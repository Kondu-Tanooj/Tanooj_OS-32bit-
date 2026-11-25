/*
 * heap.c — Kernel heap allocator (kmalloc / kfree)
 */

#include "heap.h"
#include "pmm.h"
#include "terminal.h"
#include <stddef.h>
#include <stdint.h>

typedef struct block_header {
    uint32_t             magic;
    size_t               size;
    uint8_t              free;
    struct block_header *next;
} block_header_t;

#define HEAP_MAGIC   0xDEADC0DE
#define HEAP_PAGES   16
#define HEAP_SIZE    (HEAP_PAGES * PAGE_SIZE)
#define MIN_SPLIT    (sizeof(block_header_t) + 16)

static block_header_t *heap_head = NULL;
static uint32_t        heap_base = 0;
static uint32_t        alloc_count = 0;
static uint32_t        free_count  = 0;

void heap_init(void) {
    heap_base = (uint32_t)pmm_alloc_page();
    if (!heap_base) return;

    for (int i = 1; i < HEAP_PAGES; i++) pmm_alloc_page();

    heap_head = (block_header_t *)heap_base;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size  = HEAP_SIZE - sizeof(block_header_t);
    heap_head->free  = 1;
    heap_head->next  = NULL;
}

void *kmalloc(size_t size) {
    if (!size || !heap_head) return NULL;
    size = (size + 7) & ~(size_t)7;

    block_header_t *cur = heap_head;
    while (cur) {
        if (cur->magic != HEAP_MAGIC) return NULL;
        if (cur->free && cur->size >= size) {
            if (cur->size >= size + MIN_SPLIT) {
                block_header_t *nb =
                    (block_header_t *)((uint8_t *)cur + sizeof(block_header_t) + size);
                nb->magic = HEAP_MAGIC;
                nb->size  = cur->size - size - sizeof(block_header_t);
                nb->free  = 1;
                nb->next  = cur->next;
                cur->next = nb;
                cur->size = size;
            }
            cur->free = 0;
            alloc_count++;
            return (void *)((uint8_t *)cur + sizeof(block_header_t));
        }
        cur = cur->next;
    }
    return NULL;
}

void *kmalloc_aligned(size_t size, size_t align) {
    void *p = kmalloc(size + align);
    if (!p) return NULL;
    uintptr_t addr    = (uintptr_t)p;
    uintptr_t aligned = (addr + align - 1) & ~(uintptr_t)(align - 1);
    return (void *)aligned;
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_header_t *block =
        (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    if (block->magic != HEAP_MAGIC || block->free) return;
    block->free = 1;
    free_count++;
    while (block->next && block->next->free) {
        block->size += sizeof(block_header_t) + block->next->size;
        block->next  = block->next->next;
    }
}

void heap_print_stats(void) {
    uint32_t used = 0, free = 0, blocks = 0;
    block_header_t *cur = heap_head;
    while (cur) {
        blocks++;
        if (cur->free) free += (uint32_t)cur->size;
        else           used += (uint32_t)cur->size;
        cur = cur->next;
    }
    terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    terminal_write("  Heap — Blocks: "); terminal_write_dec(blocks);
    terminal_write("  Used: ");          terminal_write_dec(used);
    terminal_write(" B  Free: ");        terminal_write_dec(free);
    terminal_write(" B  Allocs: ");      terminal_write_dec(alloc_count);
    terminal_write("  Frees: ");         terminal_write_dec(free_count);
    terminal_writeline("");
}
