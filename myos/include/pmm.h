#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096   /* 4 KiB — smallest unit we track */

/*
 * Multiboot memory map entry — passed to us by GRUB.
 * Tells us which regions of RAM are usable vs reserved.
 */
typedef struct {
    uint32_t size;        /* size of this entry (not counting this field) */
    uint64_t base;        /* start address of region */
    uint64_t length;      /* length in bytes */
    uint32_t type;        /* 1 = free/usable, anything else = reserved    */
} __attribute__((packed)) mmap_entry_t;

/* Multiboot info flags */
#define MULTIBOOT_FLAG_MMAP (1 << 6)

/* Multiboot info struct — only the fields we use */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) multiboot_info_t;

void  pmm_init(multiboot_info_t *mbi, uint32_t kernel_end);
void *pmm_alloc_page(void);
void  pmm_free_page(void *addr);
void  pmm_print_stats(void);

uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_total_pages(void);

#endif
