/*
 * mount.c — disk mount/unmount manager
 *
 * Tracks which ATA drives are mounted at which mount points.
 * mount_select() switches the active ATA drive so all subsequent
 * TFS operations use the selected disk.
 *
 * Mount points:
 *   /        → primary master (disk0, boot disk)
 *   /disk1   → primary slave
 *   /disk2   → secondary master
 *   /disk3   → secondary slave
 */

#include "mount.h"
#include "ata.h"
#include "tfs.h"
#include "terminal.h"
#include <stdint.h>

static mount_entry_t mounts[MAX_MOUNTS];
static int active_drive = ATA_PRIMARY_MASTER;
static int mount_num = 0;

static void str_copy(char *d, const char *s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]=0;
}
static int str_eq(const char *a, const char *b) {
    while(*a&&*b&&*a==*b){a++;b++;} return *a==0&&*b==0;
}

void mount_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) mounts[i].mounted = 0;
    mount_num = 0;

    /* Try all 4 ATA slots — on real hardware the drive may be on any slot */
    int drives[]  = { ATA_PRIMARY_MASTER, ATA_PRIMARY_SLAVE,
                      ATA_SECONDARY_MASTER, ATA_SECONDARY_SLAVE };
    const char *points[] = { "/", "/disk1", "/disk2", "/disk3" };
    const char *labels[] = { "disk0", "disk1", "disk2", "disk3" };

    int first = 1; /* first detected drive becomes "/" */

    for (int i = 0; i < 4; i++) {
        ata_select_drive(drives[i]);
        if (ata_detect()) {
            if (first) {
                /* First found drive = root disk */
                mount_drive(drives[i], "/", labels[i]);
                active_drive = drives[i];
                first = 0;
            } else {
                mount_drive(drives[i], points[i], labels[i]);
            }
        }
    }

    /* If nothing detected at all, force-mount primary master anyway */
    if (mount_num == 0) {
        ata_select_drive(ATA_PRIMARY_MASTER);
        active_drive = ATA_PRIMARY_MASTER;
        /* Add a dummy mount entry so TFS can try */
        str_copy(mounts[0].point, "/", MOUNT_POINT_LEN);
        str_copy(mounts[0].label, "disk0", 16);
        mounts[0].drive   = ATA_PRIMARY_MASTER;
        mounts[0].mounted = 1;
        mount_num = 1;
        tfs_init_drive();
    }

    /* Restore active drive */
    ata_select_drive(active_drive);
}

int mount_drive(int drive, const char *point, const char *label) {
    /* Check not already mounted */
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].mounted && str_eq(mounts[i].point, point))
            return -1; /* already mounted */
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].mounted) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Switch to this drive and init TFS */
    ata_select_drive(drive);
    if (!ata_detect()) {
        ata_select_drive(active_drive); /* restore */
        return -1;
    }

    /* Try to init filesystem on this drive */
    tfs_init_drive(); /* init TFS on currently selected drive */

    str_copy(mounts[slot].point, point, MOUNT_POINT_LEN);
    str_copy(mounts[slot].label, label, 16);
    mounts[slot].drive   = drive;
    mounts[slot].mounted = 1;
    mount_num++;

    /* Restore active drive */
    ata_select_drive(active_drive);
    return 0;
}

int unmount_drive(const char *point) {
    if (str_eq(point, "/")) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Cannot unmount root disk.");
        return -1;
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].mounted && str_eq(mounts[i].point, point)) {
            /* If this was the active drive, switch back to root */
            if (mounts[i].drive == active_drive) {
                ata_select_drive(ATA_PRIMARY_MASTER);
                active_drive = ATA_PRIMARY_MASTER;
                tfs_init_drive();
            }
            mounts[i].mounted = 0;
            mount_num--;
            return 0;
        }
    }
    return -1; /* not found */
}

int mount_select(const char *point) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].mounted && str_eq(mounts[i].point, point)) {
            active_drive = mounts[i].drive;
            ata_select_drive(active_drive);
            tfs_init_drive(); /* re-init TFS for this drive */
            return 0;
        }
    }
    return -1; /* mount point not found */
}

void mount_list(void) {

    static const char *drive_names[] = {
        "pri/mst", "pri/slv", "sec/mst", "sec/slv"
    };

    int found = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].mounted) continue;
        int is_active = (mounts[i].drive == active_drive);
        terminal_set_color(is_active ? COLOR_LIGHT_GREEN : COLOR_WHITE, COLOR_BLACK);
        terminal_write("  ");
        terminal_write(mounts[i].point);
        /* Pad to column 16 */
        int len = 0; while(mounts[i].point[len]) len++;
        for (int p = len; p < 13; p++) terminal_putchar(' ');
        terminal_write(drive_names[mounts[i].drive < 4 ? mounts[i].drive : 0]);
        terminal_write("   ");
        terminal_write(mounts[i].label);
        terminal_writeline(is_active ? "    [active]" : "");
        found++;
    }
    if (!found) {
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_writeline("  (no drives mounted)");
    }
}

int  mount_count(void)        { return mount_num;     }
int  mount_active_drive(void) { return active_drive;  }
