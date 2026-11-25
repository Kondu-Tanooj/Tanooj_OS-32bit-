/*
 * ata.c — ATA PIO driver with multi-drive support
 *
 * Supports 4 drives:
 *   Primary   bus (0x1F0): master (drive 0), slave (drive 1)
 *   Secondary bus (0x170): master (drive 2), slave (drive 3)
 *
 * ata_select_drive() switches the active drive.
 * All read/write ops use the currently selected drive.
 */

#include "ata.h"
#include <stdint.h>

static inline void     outb(uint16_t p, uint8_t  v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb (uint16_t p) { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw (uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }

/* Current drive state */
static int     cur_drive = ATA_PRIMARY_MASTER;
static uint16_t cur_base  = 0x1F0;
static uint8_t  cur_sel   = 0xE0;   /* 0xE0=master, 0xF0=slave */

/* Port offsets from base */
#define PORT_DATA      0
#define PORT_ERR       1
#define PORT_SECCOUNT  2
#define PORT_LBA_LO    3
#define PORT_LBA_MID   4
#define PORT_LBA_HI    5
#define PORT_DRIVE     6
#define PORT_STATUS    7
#define PORT_CMD       7

#define ATA_CMD_READ   0x20
#define ATA_CMD_WRITE  0x30
#define ATA_CMD_FLUSH  0xE7
#define ATA_CMD_ID     0xEC

#define ATA_SR_BSY     0x80
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

void ata_select_drive(int drive) {
    cur_drive = drive;
    switch (drive) {
        case ATA_PRIMARY_MASTER:
            cur_base = 0x1F0; cur_sel = 0xE0; break;
        case ATA_PRIMARY_SLAVE:
            cur_base = 0x1F0; cur_sel = 0xF0; break;
        case ATA_SECONDARY_MASTER:
            cur_base = 0x170; cur_sel = 0xE0; break;
        case ATA_SECONDARY_SLAVE:
            cur_base = 0x170; cur_sel = 0xF0; break;
    }
}

static uint8_t ata_wait_bsy(void) {
    for (int i = 0; i < 4; i++) inb(cur_base + PORT_STATUS);
    uint8_t s;
    int timeout = 1000000;
    do {
        s = inb(cur_base + PORT_STATUS);
        if (--timeout <= 0) return s; /* return even if still busy */
    } while (s & ATA_SR_BSY);
    return s;
}

static void ata_setup(uint32_t lba, uint8_t count) {
    outb(cur_base + PORT_DRIVE,    cur_sel | ((lba >> 24) & 0x0F));
    outb(cur_base + PORT_SECCOUNT, count);
    outb(cur_base + PORT_LBA_LO,   (uint8_t)(lba));
    outb(cur_base + PORT_LBA_MID,  (uint8_t)(lba >> 8));
    outb(cur_base + PORT_LBA_HI,   (uint8_t)(lba >> 16));
}

int ata_detect(void) {
    /* Select drive */
    outb(cur_base + PORT_DRIVE, cur_sel);
    /* 400ns delay */
    for (int i = 0; i < 15; i++) inb(cur_base + PORT_STATUS);

    /* Check for floating bus (0xFF = no drive) */
    uint8_t s = inb(cur_base + PORT_STATUS);
    if (s == 0xFF || s == 0x00) return 0;

    /* Send IDENTIFY */
    outb(cur_base + PORT_SECCOUNT, 0);
    outb(cur_base + PORT_LBA_LO,   0);
    outb(cur_base + PORT_LBA_MID,  0);
    outb(cur_base + PORT_LBA_HI,   0);
    outb(cur_base + PORT_CMD, ATA_CMD_ID);

    /* Poll status — if 0 immediately, no drive */
    s = inb(cur_base + PORT_STATUS);
    if (s == 0) return 0;

    /* Wait for BSY to clear with timeout */
    int timeout = 100000;
    while ((inb(cur_base + PORT_STATUS) & ATA_SR_BSY) && timeout-- > 0);
    if (timeout <= 0) return 0;

    /* Check LBA mid/hi — non-zero means ATAPI or other non-ATA device */
    uint8_t mid = inb(cur_base + PORT_LBA_MID);
    uint8_t hi  = inb(cur_base + PORT_LBA_HI);
    if (mid != 0 || hi != 0) return 0; /* not a plain ATA disk */

    /* Wait for DRQ or ERR */
    int tries = 100000;
    while (tries-- > 0) {
        s = inb(cur_base + PORT_STATUS);
        if (s & ATA_SR_ERR) return 0;
        if (s & ATA_SR_DRQ) return 1; /* drive present and ready */
    }
    return 0;
}

int ata_detect_drive(int drive) {
    ata_select_drive(drive);
    return ata_detect();
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    ata_wait_bsy();
    ata_setup(lba, count);
    outb(cur_base + PORT_CMD, ATA_CMD_READ);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        uint8_t st;
        int timeout = 500000;
        do {
            st = inb(cur_base + PORT_STATUS);
            if (st & ATA_SR_ERR) return -1;
            if (--timeout <= 0) return -1;
        } while (!(st & ATA_SR_DRQ));
        for (int i = 0; i < 256; i++) *ptr++ = inw(cur_base + PORT_DATA);
    }
    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    ata_wait_bsy();
    ata_setup(lba, count);
    outb(cur_base + PORT_CMD, ATA_CMD_WRITE);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        uint8_t st;
        int timeout = 500000; /* prevent infinite hang on read-only media */
        do {
            st = inb(cur_base + PORT_STATUS);
            if (st & ATA_SR_ERR) return -1;
            if (--timeout <= 0) return -1; /* timeout — read-only or no disk */
        } while (!(st & ATA_SR_DRQ));
        for (int i = 0; i < 256; i++) outw(cur_base + PORT_DATA, *ptr++);
    }
    outb(cur_base + PORT_CMD, ATA_CMD_FLUSH);
    ata_wait_bsy();
    return 0;
}
