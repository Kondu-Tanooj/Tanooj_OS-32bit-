/*
 * tfs.c — TanoojFS filesystem driver
 *
 * Sits on top of the ATA driver. Manages a flat file table on disk.
 * All metadata fits in sectors 0-8. File data starts at sector 9.
 *
 * Allocation strategy: sectors are assigned sequentially from a
 * free-sector pointer. Deleted files leave gaps (no defrag yet).
 * A fresh format clears everything and resets the pointer to sector 9.
 */

#include "tfs.h"
#include "ata.h"
#include "terminal.h"
#include "heap.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   String helpers (no libc)
   ----------------------------------------------------------------------- */
static int tfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void tfs_strcpy(char *d, const char *s, int max) {
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void tfs_memcpy(void *d, const void *s, int n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
}
static void tfs_memset(void *d, uint8_t v, int n) {
    uint8_t *dd = (uint8_t *)d;
    for (int i = 0; i < n; i++) dd[i] = v;
}

/* -----------------------------------------------------------------------
   State
   ----------------------------------------------------------------------- */
static int      mounted     = 0;
static uint32_t next_sector = TFS_DATA_START; /* next free data sector */

/* Sector-aligned buffers for ATA (must be multiple of 512 bytes) */
static uint8_t  sect_buf[SECTOR_SIZE];

/* -----------------------------------------------------------------------
   Read/write the superblock
   ----------------------------------------------------------------------- */
static int read_super(tfs_super_t *sb) {
    if (ata_read(0, 1, sect_buf) != 0) return -1;
    tfs_memcpy(sb, sect_buf, sizeof(tfs_super_t));
    return 0;
}
static int write_super(tfs_super_t *sb) {
    tfs_memcpy(sect_buf, sb, sizeof(tfs_super_t));
    return ata_write(0, 1, sect_buf);
}

/* -----------------------------------------------------------------------
   Read/write a specific file entry (index 0..TFS_MAX_FILES-1)
   Each sector holds 8 entries (512 / 64 = 8)
   ----------------------------------------------------------------------- */
static int read_entry(int idx, tfs_entry_t *e) {
    int sector = 1 + idx / 8;  /* entries live in sectors 1-8 */
    int slot   = idx % 8;
    if (ata_read(sector, 1, sect_buf) != 0) return -1;
    tfs_memcpy(e, sect_buf + slot * sizeof(tfs_entry_t), sizeof(tfs_entry_t));
    return 0;
}
static int write_entry(int idx, tfs_entry_t *e) {
    int sector = 1 + idx / 8;
    int slot   = idx % 8;
    if (ata_read(sector, 1, sect_buf) != 0) return -1;
    tfs_memcpy(sect_buf + slot * sizeof(tfs_entry_t), e, sizeof(tfs_entry_t));
    return ata_write(sector, 1, sect_buf);
}

/* -----------------------------------------------------------------------
   tfs_format — wipe disk and write a fresh superblock + empty file table
   ----------------------------------------------------------------------- */
int tfs_format(void) {
    /* Write empty superblock */
    tfs_super_t sb;
    tfs_memset(&sb, 0, sizeof(sb));
    sb.magic      = TFS_MAGIC;
    sb.version    = TFS_VERSION;
    sb.file_count = 0;
    if (write_super(&sb) != 0) return -1;

    /* Clear all file table sectors (1-8) */
    tfs_memset(sect_buf, 0, SECTOR_SIZE);
    for (int s = 1; s <= 8; s++)
        if (ata_write(s, 1, sect_buf) != 0) return -1;

    next_sector = TFS_DATA_START;
    mounted     = 1;
    return 0;
}

/* -----------------------------------------------------------------------
   tfs_init — check for valid superblock, format if needed
   ----------------------------------------------------------------------- */
int tfs_init(void) {
    if (!ata_detect()) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  [TFS] No disk detected.");
        return -1;
    }

    tfs_super_t sb;
    if (read_super(&sb) != 0) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  [TFS] Disk read error.");
        return -1;
    }

    if (sb.magic != TFS_MAGIC) {
        /* First boot — format the disk */
        if (tfs_format() != 0) return -1;
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_writeline("  [TFS] Disk formatted. TanoojFS ready.");
    } else {
        /* Existing filesystem — find next_sector */
        next_sector = TFS_DATA_START;
        for (int i = 0; i < TFS_MAX_FILES; i++) {
            tfs_entry_t e;
            if (read_entry(i, &e) == 0 && e.used) {
                uint32_t end = e.start_sector + e.sector_count;
                if (end > next_sector) next_sector = end;
            }
        }
        mounted = 1;
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  [TFS] Mounted. Files: ");
        terminal_write_dec(sb.file_count);
        terminal_writeline("");
    }
    return 0;
}

int  tfs_is_mounted(void) { return mounted; }
int  tfs_file_count(void) {
    tfs_super_t sb;
    if (read_super(&sb) != 0) return 0;
    return (int)sb.file_count;
}

/* -----------------------------------------------------------------------
   tfs_save_file — write file to disk (creates or overwrites)
   ----------------------------------------------------------------------- */
int tfs_save_file(const char *name, const char *data, uint32_t size) {
    if (!mounted) return -1;

    /* Find existing entry or empty slot */
    int      slot   = -1;
    int      exists = 0;
    uint32_t old_start = 0, old_sects = 0;

    for (int i = 0; i < TFS_MAX_FILES; i++) {
        tfs_entry_t e;
        if (read_entry(i, &e) != 0) continue;
        if (e.used && tfs_strcmp(e.name, name) == 0) {
            slot      = i;
            exists    = 1;
            old_start = e.start_sector;
            old_sects = e.sector_count;
            break;
        }
        if (!e.used && slot == -1) slot = i;
    }

    if (slot == -1) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  [TFS] File table full!");
        return -1;
    }

    /* Calculate sectors needed */
    uint32_t needed = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (needed == 0) needed = 1;

    /* Allocate sectors — reuse old slot if same size, else append */
    uint32_t start;
    if (exists && old_sects >= needed) {
        start = old_start; /* reuse existing allocation */
    } else {
        start = next_sector;
        next_sector += needed;
        if (next_sector >= TFS_DISK_SECTS) {
            terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            terminal_writeline("  [TFS] Disk full!");
            return -1;
        }
    }

    /* Write data sector by sector */
    uint32_t remaining = size;
    uint32_t offset    = 0;
    for (uint32_t s = 0; s < needed; s++) {
        tfs_memset(sect_buf, 0, SECTOR_SIZE);
        uint32_t chunk = remaining > SECTOR_SIZE ? SECTOR_SIZE : remaining;
        if (chunk > 0) tfs_memcpy(sect_buf, data + offset, chunk);
        if (ata_write(start + s, 1, sect_buf) != 0) return -1;
        offset    += chunk;
        remaining -= chunk;
    }

    /* Write file entry */
    tfs_entry_t e;
    tfs_memset(&e, 0, sizeof(e));
    tfs_strcpy(e.name, name, TFS_NAME_MAX);
    e.start_sector = start;
    e.size         = size;
    e.sector_count = needed;
    e.used         = 1;
    if (write_entry(slot, &e) != 0) return -1;

    /* Update superblock */
    tfs_super_t sb;
    if (read_super(&sb) != 0) return -1;
    if (!exists) sb.file_count++;
    write_super(&sb);

    return 0;
}

/* -----------------------------------------------------------------------
   tfs_load_file — read file from disk into buf
   ----------------------------------------------------------------------- */
int tfs_load_file(const char *name, char *buf, uint32_t buf_size) {
    if (!mounted) return -1;

    for (int i = 0; i < TFS_MAX_FILES; i++) {
        tfs_entry_t e;
        if (read_entry(i, &e) != 0) continue;
        if (!e.used || tfs_strcmp(e.name, name) != 0) continue;

        /* Found it — read sectors */
        uint32_t to_read = e.size < buf_size ? e.size : buf_size - 1;
        uint32_t offset  = 0;
        for (uint32_t s = 0; s < e.sector_count; s++) {
            if (ata_read(e.start_sector + s, 1, sect_buf) != 0) return -1;
            uint32_t chunk = to_read - offset;
            if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
            tfs_memcpy(buf + offset, sect_buf, chunk);
            offset += chunk;
            if (offset >= to_read) break;
        }
        buf[to_read] = 0;
        return (int)to_read;
    }
    return -1; /* not found */
}

/* -----------------------------------------------------------------------
   tfs_delete_file — mark entry as unused
   ----------------------------------------------------------------------- */
int tfs_delete_file(const char *name) {
    if (!mounted) return -1;

    for (int i = 0; i < TFS_MAX_FILES; i++) {
        tfs_entry_t e;
        if (read_entry(i, &e) != 0) continue;
        if (!e.used || tfs_strcmp(e.name, name) != 0) continue;

        e.used = 0;
        if (write_entry(i, &e) != 0) return -1;

        tfs_super_t sb;
        if (read_super(&sb) == 0) {
            if (sb.file_count > 0) sb.file_count--;
            write_super(&sb);
        }
        return 0;
    }
    return -1;
}

/* -----------------------------------------------------------------------
   tfs_list — print all files to terminal
   ----------------------------------------------------------------------- */
int tfs_list(void) {
    if (!mounted) { terminal_writeline("  [TFS] Not mounted."); return -1; }

    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  Disk files (TanoojFS):");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);

    int count = 0;
    for (int i = 0; i < TFS_MAX_FILES; i++) {
        tfs_entry_t e;
        if (read_entry(i, &e) != 0) continue;
        if (!e.used) continue;
        terminal_write("    ");
        terminal_write(e.name);
        terminal_write("  (");
        terminal_write_dec(e.size);
        terminal_writeline(" B)");
        count++;
    }
    if (count == 0) {
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_writeline("    (no files saved to disk yet)");
    }
    return count;
}

/* Re-initialise TFS on the currently selected ATA drive */
void tfs_init_drive(void) {
    tfs_init();
}
