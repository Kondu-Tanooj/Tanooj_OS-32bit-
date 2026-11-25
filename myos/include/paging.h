#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stddef.h>

/*
 * x86 page directory entry flags
 */
#define PAGE_PRESENT    (1 << 0)   /* page is present in memory   */
#define PAGE_WRITABLE   (1 << 1)   /* page is writable            */
#define PAGE_USER       (1 << 2)   /* accessible from ring 3      */
#define PAGE_4MB        (1 << 7)   /* 4 MiB page (PSE)            */

/* Page size */
#define PAGE_SIZE       4096

/* Number of entries in a page directory or page table */
#define PAGE_DIR_ENTRIES  1024
#define PAGE_TBL_ENTRIES  1024

/* Extract indices from a virtual address */
#define PD_INDEX(va)   (((va) >> 22) & 0x3FF)  /* bits 31-22 */
#define PT_INDEX(va)   (((va) >> 12) & 0x3FF)  /* bits 21-12 */
#define PG_OFFSET(va)  ((va) & 0xFFF)           /* bits 11-0  */

/* Align an address up to the next page boundary */
#define PAGE_ALIGN_UP(a)   (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(a) ((a) & ~(PAGE_SIZE - 1))

/*
 * Initialise paging — build page directory, identity-map first 4 MiB,
 * install page fault handler, load CR3, enable paging bit in CR0.
 */
void paging_init(void);

/*
 * Map a virtual address to a physical address in the current page directory.
 * flags: PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER as needed.
 */
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);

/*
 * Unmap a virtual address (mark the PTE as not present).
 */
void paging_unmap(uint32_t virt);

/*
 * Translate virtual → physical. Returns 0xFFFFFFFF if not mapped.
 */
uint32_t paging_virt_to_phys(uint32_t virt);

/*
 * Print paging stats to terminal.
 */
void paging_print_stats(void);

/*
 * Returns 1 if paging is enabled.
 */
int paging_enabled(void);

#endif
