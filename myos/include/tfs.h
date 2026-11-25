#ifndef TFS_H
#define TFS_H

/*
 * TanoojFS — simple flat filesystem layout on disk:
 *
 * Sector 0       : Superblock (magic, version, file count)
 * Sectors 1-8    : File table (up to 64 file entries x 64 bytes each)
 * Sectors 9+     : File data (each file gets a contiguous run of sectors)
 *
 * Max files : 64
 * Max file  : 255 sectors = 130,560 bytes (~127 KiB)
 * Total disk: 32 MiB (65536 sectors)
 */

#include <stdint.h>

#define TFS_MAGIC       0x544E4653   /* "TNFS" */
#define TFS_VERSION     1
#define TFS_MAX_FILES   64
#define TFS_NAME_MAX    48
#define TFS_DATA_START  9            /* first data sector */
#define TFS_DISK_SECTS  65536        /* total sectors on disk */

/* Superblock — lives at sector 0 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t reserved[125];  /* pad to 512 bytes */
} __attribute__((packed)) tfs_super_t;

/* File entry — 64 bytes, 8 fit per sector, 64 total in sectors 1-8 */
typedef struct {
    char     name[TFS_NAME_MAX];  /* filename                      */
    uint32_t start_sector;        /* first data sector             */
    uint32_t size;                /* size in bytes                 */
    uint32_t sector_count;        /* sectors allocated             */
    uint8_t  used;                /* 1 = valid entry               */
    uint8_t  reserved[7];
} __attribute__((packed)) tfs_entry_t;

/* Public API */
int  tfs_init(void);                         /* mount or format disk         */
void tfs_init_drive(void);                   /* re-init TFS on current drive  */
int  tfs_format(void);                       /* wipe and create fresh fs     */
int  tfs_save_file(const char *name, const char *data, uint32_t size);
int  tfs_load_file(const char *name, char *buf, uint32_t buf_size);
int  tfs_delete_file(const char *name);
int  tfs_list(void);                         /* print all files to terminal  */
int  tfs_file_count(void);
int  tfs_is_mounted(void);

#endif
