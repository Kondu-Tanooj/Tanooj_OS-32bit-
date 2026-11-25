#ifndef MOUNT_H
#define MOUNT_H

#include "ata.h"
#include <stdint.h>

#define MAX_MOUNTS   4
#define MOUNT_POINT_LEN 16

typedef struct {
    char    point[MOUNT_POINT_LEN]; /* e.g. "/", "/disk2"  */
    int     drive;                  /* ATA_PRIMARY_MASTER etc */
    int     mounted;                /* 1 = active             */
    char    label[16];              /* friendly name           */
} mount_entry_t;

void  mount_init(void);
int   mount_drive(int drive, const char *point, const char *label);
int   unmount_drive(const char *point);
int   mount_select(const char *point); /* switch active drive   */
void  mount_list(void);
int   mount_count(void);

/* Get currently active drive number */
int   mount_active_drive(void);

#endif
