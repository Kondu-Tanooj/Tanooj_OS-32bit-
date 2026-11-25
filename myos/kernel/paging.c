/*
 * paging.c — x86 32-bit paging
 *
 * Strategy:
 *   1. Allocate a page directory (4 KiB, page-aligned)
 *   2. Identity-map 0x00000000–0x003FFFFF (first 4 MiB) — covers:
 *        0x000000–0x0FFFFF  BIOS, VGA buffer (0xB8000 is here)
 *        0x100000–0x3FFFFF  Kernel code + data + stack + heap
 *      We use 4 MiB pages (PSE) for simplicity — one PDE per 4 MiB,
 *      no page table needed.
 *   3. Load CR3 with the page directory physical address
 *   4. Set PSE bit in CR4 (enables 4 MiB pages)
 *   5. Set PG bit in CR0 — paging is now LIVE
 *
 * After init, the kernel runs exactly as before but now through the MMU.
 * The page fault handler (IDT entry 14) catches bad accesses.
 */

#include "paging.h"
#include "pmm.h"
#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   Page directory — must be 4 KiB aligned.
   We put it in BSS so it's aligned by the linker (.bss ALIGN(4K)).
   ----------------------------------------------------------------------- */
static uint32_t page_directory[PAGE_DIR_ENTRIES] __attribute__((aligned(4096)));

/* We allocate page tables dynamically from PMM when needed */
static uint32_t *page_tables[PAGE_DIR_ENTRIES]; /* pointers to allocated PTs */

static int is_paging_enabled = 0;
static uint32_t mapped_pages  = 0;

/* -----------------------------------------------------------------------
   Zero memory helper (no libc)
   ----------------------------------------------------------------------- */
static void memzero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < size; i++) p[i] = 0;
}

/* -----------------------------------------------------------------------
   paging_map — map virt → phys with given flags
   ----------------------------------------------------------------------- */
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    /* If this PDE uses 4 MiB pages, we can't add a page table entry */
    if (page_directory[pd_idx] & PAGE_4MB) return;

    /* Allocate page table if not present */
    if (!page_tables[pd_idx]) {
        uint32_t *pt = (uint32_t *)pmm_alloc_page();
        if (!pt) return;
        memzero(pt, PAGE_SIZE);
        page_tables[pd_idx] = pt;
        /* Install into page directory */
        page_directory[pd_idx] = (uint32_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Set the page table entry */
    page_tables[pd_idx][pt_idx] = (phys & ~0xFFF) | (flags | PAGE_PRESENT);
    mapped_pages++;

    /* Invalidate TLB entry for this virtual address */
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* -----------------------------------------------------------------------
   paging_unmap
   ----------------------------------------------------------------------- */
void paging_unmap(uint32_t virt) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    if (!page_tables[pd_idx]) return;
    if (!(page_tables[pd_idx][pt_idx] & PAGE_PRESENT)) return;

    page_tables[pd_idx][pt_idx] = 0;
    if (mapped_pages > 0) mapped_pages--;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* -----------------------------------------------------------------------
   paging_virt_to_phys
   ----------------------------------------------------------------------- */
uint32_t paging_virt_to_phys(uint32_t virt) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PAGE_PRESENT)) return 0xFFFFFFFF;

    /* 4 MiB page */
    if (page_directory[pd_idx] & PAGE_4MB) {
        uint32_t base = page_directory[pd_idx] & 0xFFC00000;
        return base | (virt & 0x3FFFFF);
    }

    if (!page_tables[pd_idx]) return 0xFFFFFFFF;
    uint32_t pte = page_tables[pd_idx][pt_idx];
    if (!(pte & PAGE_PRESENT)) return 0xFFFFFFFF;
    return (pte & ~0xFFF) | PG_OFFSET(virt);
}

/* -----------------------------------------------------------------------
   paging_init
   ----------------------------------------------------------------------- */
void paging_init(void) {
    /* Zero page directory and table pointers */
    memzero(page_directory, sizeof(page_directory));
    memzero(page_tables,    sizeof(page_tables));
    mapped_pages = 0;

    /*
     * Identity-map the first 4 MiB using a 4 MiB PSE page.
     * This covers:
     *   - Interrupt vectors, BIOS data (low memory)
     *   - VGA buffer at 0xB8000
     *   - Kernel code and data (0x100000+)
     *   - Kernel stack and heap
     *
     * PDE flags: Present | Writable | 4MiB page
     */
    page_directory[0] = 0x00000000 | PAGE_PRESENT | PAGE_WRITABLE | PAGE_4MB;

    /*
     * Also identity-map the second 4 MiB (0x400000–0x7FFFFF)
     * for the heap which may grow beyond 4 MiB.
     */
    page_directory[1] = 0x00400000 | PAGE_PRESENT | PAGE_WRITABLE | PAGE_4MB;

    /*
     * Enable PSE (Page Size Extensions) in CR4.
     * This allows 4 MiB pages — without this, the PAGE_4MB flag is ignored.
     */
    __asm__ volatile (
        "mov %%cr4, %%eax\n"
        "or  $0x10,  %%eax\n"   /* bit 4 = PSE */
        "mov %%eax, %%cr4\n"
        ::: "eax"
    );

    /*
     * Load CR3 with the physical address of our page directory.
     * Since we haven't enabled paging yet, virtual == physical here.
     */
    __asm__ volatile (
        "mov %0, %%cr3\n"
        :: "r"((uint32_t)page_directory)
    );

    /*
     * Enable paging: set bit 31 (PG) in CR0.
     * From this instruction onwards, all addresses go through the MMU.
     */
    __asm__ volatile (
        "mov %%cr0, %%eax\n"
        "or  $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        ::: "eax"
    );

    is_paging_enabled = 1;

}

/* -----------------------------------------------------------------------
   paging_print_stats
   ----------------------------------------------------------------------- */
void paging_print_stats(void) {
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  Paging status:");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);

    terminal_write("    Status     : ");
    terminal_set_color(is_paging_enabled ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED, COLOR_BLACK);
    terminal_writeline(is_paging_enabled ? "enabled" : "disabled");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);

    terminal_writeline("    Mode       : 32-bit protected, 4 KiB + 4 MiB pages");
    terminal_writeline("    0x00000000 : identity mapped (4 MiB) — BIOS + VGA + kernel");
    terminal_writeline("    0x00400000 : identity mapped (4 MiB) — heap overflow");
    terminal_writeline("    0x00800000+: unmapped (page fault on access)");

    terminal_write("    Extra maps : ");
    terminal_write_dec(mapped_pages);
    terminal_writeline(" pages");
}

int paging_enabled(void) { return is_paging_enabled; }
