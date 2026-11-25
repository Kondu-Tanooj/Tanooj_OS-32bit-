/*
 * auth.c — TanoojOS boot password
 *
 * Flow:
 *   1. "Press any key" screen — flushes ALL stale GRUB keypresses
 *   2. Password screen — drawn AFTER flush, waits for real input
 *   3. 3 attempts, then halt
 *
 * Password hash stored at disk sector 100.
 * Default password: tanooj
 */

#include "auth.h"
#include "ata.h"
#include "terminal.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
   Port I/O
   ----------------------------------------------------------------------- */
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}

/* -----------------------------------------------------------------------
   VGA
   ----------------------------------------------------------------------- */
static volatile uint16_t * const VGA = (volatile uint16_t *)0xB8000;

static void vga_fill(int row, uint8_t color) {
    for (int i = 0; i < 80; i++)
        VGA[row * 80 + i] = ' ' | ((uint16_t)color << 8);
}

static void vga_str_at(int row, int col, const char *s, uint8_t color) {
    for (int i = 0; s[i] && col + i < 80; i++)
        VGA[row * 80 + col + i] = (uint16_t)(unsigned char)s[i] | ((uint16_t)color << 8);
}

static void vga_str_center(int row, const char *s, uint8_t color) {
    int len = 0; while (s[len]) len++;
    int col = (80 - len) / 2;
    vga_str_at(row, col, s, color);
}

static void screen_clear(void) {
    for (int i = 0; i < 80 * 25; i++)
        VGA[i] = ' ' | (0x00 << 8);
}

/* -----------------------------------------------------------------------
   Hash
   ----------------------------------------------------------------------- */
static uint32_t hash_password(const char *pass) {
    uint32_t h = 0;
    for (int i = 0; pass[i]; i++)
        h = (h * 31 + (uint8_t)pass[i]) ^ (uint32_t)(i * 0x5A3);
    return h ^ 0xDEAD1234;
}

/* -----------------------------------------------------------------------
   Disk storage
   ----------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;
    uint32_t hash;
    uint8_t  pad[504];
} __attribute__((packed)) auth_sector_t;

#define AUTH_MAGIC  0xA07BEED5
#define AUTH_SECTOR 100

static uint8_t auth_buf[512];

static int load_hash(uint32_t *out) {
    if (ata_read(AUTH_SECTOR, 1, auth_buf) != 0) return -1;
    auth_sector_t *s = (auth_sector_t *)auth_buf;
    if (s->magic != AUTH_MAGIC) return -1;
    *out = s->hash;
    return 0;
}

static int save_hash(uint32_t hash) {
    for (int i = 0; i < 512; i++) auth_buf[i] = 0;
    auth_sector_t *s = (auth_sector_t *)auth_buf;
    s->magic = AUTH_MAGIC;
    s->hash  = hash;
    return ata_write(AUTH_SECTOR, 1, auth_buf);
}

/* -----------------------------------------------------------------------
   Scancode → ASCII
   ----------------------------------------------------------------------- */
static const char sc_ascii[58] = {
    0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,0,
    'q','w','e','r','t','y','u','i','o','p','[',']',0,0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' '
};

/* -----------------------------------------------------------------------
   Stage 1: Silent drain — no interaction, just wait and flush everything
   Works for both -kernel and ISO boot paths.
   ----------------------------------------------------------------------- */
/* Read seconds from CMOS RTC (port 0x70/0x71) */
static uint8_t rtc_seconds(void) {
    outb(0x70, 0x00);           /* select seconds register */
    uint8_t bcd = inb(0x71);    /* read BCD value */
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static void silent_drain(void) {
    /* Disable IRQs for entire auth session */
    __asm__ volatile ("cli");

    screen_clear();
    vga_str_center(12, "TanoojOS v0.1", 0x0B);
    vga_str_center(13, "Loading...", 0x08);

    /* Wait 3 real seconds using RTC — hardware guaranteed, not CPU cycles */
    uint8_t start = rtc_seconds();
    uint8_t now   = start;
    uint8_t elapsed = 0;

    while (elapsed < 3) {
        /* Drain keyboard buffer continuously while waiting */
        while (inb(0x64) & 0x01) inb(0x60);

        now = rtc_seconds();
        /* Handle minute rollover (59 -> 0) */
        if (now >= start)
            elapsed = (uint8_t)(now - start);
        else
            elapsed = (uint8_t)(60 - start + now);
    }

    /* Final drain after wait */
    while (inb(0x64) & 0x01) inb(0x60);
}

/* -----------------------------------------------------------------------
   Stage 2: Draw password screen
   ----------------------------------------------------------------------- */
static void draw_password_screen(int attempt, int wrong) {
    screen_clear();

    /* Title bar */
    vga_fill(7,  0x1F);
    vga_fill(8,  0x1F);
    vga_fill(9,  0x1F);
    vga_str_center(7, "",             0x1F);
    vga_str_center(8, "  TanoojOS  ", 0x1F);
    vga_str_center(9, "Secure Login", 0x1B);

    /* Prompt */
    vga_str_center(12, "Enter boot password:", 0x07);

    /* Input field — show clearly */
    vga_str_at(14, 28, "[                    ]", 0x70);

    /* Default password hint */
    vga_str_center(16, "Default password: tanooj", 0x08);

    /* Attempt info */
    if (attempt > 1 || wrong) {
        char msg[] = "Attempt X of 3  (wrong password)";
        msg[8] = '0' + (char)attempt;
        if (!wrong) msg[10] = 0; /* trim if not wrong yet */
        vga_str_center(18, wrong ? msg : "Attempt X of 3", 0x0C);
        if (!wrong) {
            char a[] = "Attempt X of 3";
            a[8] = '0' + (char)attempt;
            vga_str_center(18, a, 0x0E);
        }
    }

    /* Instructions */
    vga_str_center(21, "Type password and press Enter", 0x08);
    vga_str_center(22, "3 wrong attempts = system halted", 0x08);
}

/* -----------------------------------------------------------------------
   Stage 2: Read password with * masking
   ----------------------------------------------------------------------- */
#define PASS_MAX 32
#define INPUT_COL 29
#define INPUT_ROW 14

static int read_password(char *buf, int max) {
    int len = 0;

    /* Clear input field */
    for (int i = 0; i < 20; i++)
        VGA[INPUT_ROW * 80 + INPUT_COL + i] = ' ' | (0x70 << 8);

    while (1) {
        while (!(inb(0x64) & 0x01));
        uint8_t sc = inb(0x60);
        if (sc & 0x80) continue; /* skip key releases */

        if (sc == 0x1C) { /* Enter */
            if (len == 0) continue; /* never accept empty */
            buf[len] = 0;
            return len;
        }
        if (sc == 0x0E) { /* Backspace */
            if (len > 0) {
                len--;
                VGA[INPUT_ROW * 80 + INPUT_COL + len] = ' ' | (0x70 << 8);
            }
            continue;
        }
        if (sc == 0x01) { /* Escape — clear field */
            len = 0;
            for (int i = 0; i < 20; i++)
                VGA[INPUT_ROW * 80 + INPUT_COL + i] = ' ' | (0x70 << 8);
            continue;
        }
        if (sc < 58) {
            char c = sc_ascii[sc];
            if (c && len < max - 1) {
                VGA[INPUT_ROW * 80 + INPUT_COL + len] = '*' | (0x70 << 8);
                buf[len++] = c;
            }
        }
    }
}

/* -----------------------------------------------------------------------
   auth_boot
   ----------------------------------------------------------------------- */
int auth_boot(void) {
    ata_select_drive(ATA_PRIMARY_MASTER);

    uint32_t stored_hash;
    if (load_hash(&stored_hash) != 0) {
        stored_hash = hash_password("tanooj");
        save_hash(stored_hash);
    }

    /* Stage 1: silent drain — no interaction */
    silent_drain();

    /* Stage 2: password prompt loop */
    char pass[PASS_MAX];
    int attempt = 0;

    while (attempt < 3) {
        draw_password_screen(attempt + 1, attempt > 0);
        read_password(pass, PASS_MAX);

        if (hash_password(pass) == stored_hash) {
            screen_clear();
            __asm__ volatile ("sti");
            return 1;
        }
        attempt++;
    }

    /* 3 failures — halt */
    screen_clear();
    for (int r = 9; r <= 15; r++) vga_fill(r, 0x40);
    vga_str_center(11, "  ACCESS DENIED  ", 0x4F);
    vga_str_center(13, "Too many failed attempts.", 0x4C);
    vga_str_center(14, "System halted permanently.", 0x4C);

    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* -----------------------------------------------------------------------
   auth_set_password
   ----------------------------------------------------------------------- */
void auth_set_password(const char *newpass) {
    uint32_t h = hash_password(newpass);
    if (save_hash(h) == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_writeline("  Password updated successfully.");
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_writeline("  New password required on next boot.");
    } else {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Failed to save password to disk.");
        terminal_set_color(COLOR_LIGHT_BROWN, COLOR_BLACK);
        terminal_writeline("  Note: passwd requires a writable disk.");
        terminal_writeline("  When booting from ISO without a disk,");
        terminal_writeline("  password changes cannot be saved.");
    }
}
