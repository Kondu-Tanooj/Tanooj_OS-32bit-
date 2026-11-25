#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/*
 * GDT — Global Descriptor Table
 *
 * The CPU uses the GDT to define memory segments and privilege rings.
 * We need 3 entries:
 *   0 — Null descriptor  (required by CPU spec, must be all zeros)
 *   1 — Kernel code      (ring 0, executable)
 *   2 — Kernel data      (ring 0, read/write)
 *
 * Each entry is 8 bytes packed into this structure.
 */
typedef struct {
    uint16_t limit_low;    /* Segment limit bits 0–15  */
    uint16_t base_low;     /* Base address bits 0–15   */
    uint8_t  base_mid;     /* Base address bits 16–23  */
    uint8_t  access;       /* Access flags (ring, type) */
    uint8_t  granularity;  /* Limit bits 16–19 + flags  */
    uint8_t  base_high;    /* Base address bits 24–31  */
} __attribute__((packed)) gdt_entry_t;

/* Pointer structure loaded into the CPU via lgdt instruction */
typedef struct {
    uint16_t limit;        /* Size of GDT in bytes minus 1 */
    uint32_t base;         /* Linear address of GDT         */
} __attribute__((packed)) gdt_ptr_t;

void gdt_init(void);

#endif
