#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define SECTOR_SIZE 512

/* Drive selection */
#define ATA_PRIMARY_MASTER   0
#define ATA_PRIMARY_SLAVE    1
#define ATA_SECONDARY_MASTER 2
#define ATA_SECONDARY_SLAVE  3

/* Select which drive to operate on */
void ata_select_drive(int drive);
int  ata_detect_drive(int drive);  /* returns 1 if present */

int  ata_read (uint32_t lba, uint8_t count, void *buf);
int  ata_write(uint32_t lba, uint8_t count, const void *buf);
int  ata_detect(void);  /* detect current selected drive */

#endif
