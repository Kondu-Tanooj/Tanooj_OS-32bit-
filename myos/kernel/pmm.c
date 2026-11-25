/*
 * pmm.c — Physical Memory Manager
 */

#include "pmm.h"
#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_PAGES    (1024 * 1024)
#define BITMAP_SIZE  (MAX_PAGES / 32)

static uint32_t bitmap[BITMAP_SIZE];
static uint32_t total_pages = 0;
static uint32_t free_pages  = 0;

static inline void bitmap_set(uint32_t page)   { bitmap[page/32] |=  (1u << (page%32)); }
static inline void bitmap_clear(uint32_t page) { bitmap[page/32] &= ~(1u << (page%32)); }
static inline int  bitmap_test(uint32_t page)  { return (bitmap[page/32] >> (page%32)) & 1; }

static void pmm_free_region(uint64_t base, uint64_t length) {
    uint64_t start = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (base + length)        & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t page = (uint32_t)(addr / PAGE_SIZE);
        if (page < MAX_PAGES && bitmap_test(page)) {
            bitmap_clear(page);
            free_pages++;
        }
    }
}

static void pmm_use_region(uint64_t base, uint64_t length) {
    uint32_t start_page = (uint32_t)(base / PAGE_SIZE);
    uint32_t num_pages  = (uint32_t)((length + PAGE_SIZE - 1) / PAGE_SIZE);
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page = start_page + i;
        if (page < MAX_PAGES && !bitmap_test(page)) {
            bitmap_set(page);
            if (free_pages > 0) free_pages--;
        }
    }
}

extern uint32_t _kernel_end;

void pmm_init(multiboot_info_t *mbi, uint32_t kernel_end) {
    (void)kernel_end;

    for (int i = 0; i < (int)BITMAP_SIZE; i++) bitmap[i] = 0xFFFFFFFF;
    total_pages = 0;
    free_pages  = 0;

    if (!mbi || !(mbi->flags & MULTIBOOT_FLAG_MMAP)) return;

    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
    mmap_entry_t *entry = (mmap_entry_t *)mbi->mmap_addr;

    while ((uint32_t)entry < mmap_end) {
        if (entry->type == 1) {
            total_pages += (uint32_t)(entry->length / PAGE_SIZE);
            pmm_free_region(entry->base, entry->length);
        }
        entry = (mmap_entry_t *)((uint32_t)entry + entry->size + sizeof(uint32_t));
    }

    pmm_use_region(0, PAGE_SIZE);
    uint32_t kend = (uint32_t)&_kernel_end;
    pmm_use_region(0x100000, kend - 0x100000 + PAGE_SIZE);
}

void *pmm_alloc_page(void) {
    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        if (bitmap[i] == 0xFFFFFFFF) continue;
        for (int bit = 0; bit < 32; bit++) {
            if (!((bitmap[i] >> bit) & 1)) {
                uint32_t page = i * 32 + bit;
                bitmap_set(page);
                if (free_pages > 0) free_pages--;
                return (void *)(page * PAGE_SIZE);
            }
        }
    }
    return NULL;
}

void pmm_free_page(void *addr) {
    uint32_t page = (uint32_t)addr / PAGE_SIZE;
    if (page < MAX_PAGES && bitmap_test(page)) {
        bitmap_clear(page);
        free_pages++;
    }
}

uint32_t pmm_get_free_pages(void)  { return free_pages;  }
uint32_t pmm_get_total_pages(void) { return total_pages; }

void pmm_print_stats(void) {
    terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    terminal_write("  RAM — Total: ");
    terminal_write_dec(total_pages * 4);
    terminal_write(" KiB  Free: ");
    terminal_write_dec(free_pages * 4);
    terminal_write(" KiB  Used: ");
    terminal_write_dec((total_pages - free_pages) * 4);
    terminal_writeline(" KiB");
}
