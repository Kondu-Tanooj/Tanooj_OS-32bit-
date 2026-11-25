#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Simple free-list heap allocator.
 *
 * The heap is a contiguous region of memory divided into blocks.
 * Each block has a small header followed by usable data.
 *
 * Block header:
 *   [size][free][next] → [user data ...]
 *
 * kmalloc(n) — find a free block big enough, split it if too large
 * kfree(p)   — mark block free, merge adjacent free blocks
 */

void  heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void  kfree(void *ptr);
void  heap_print_stats(void);

#endif
