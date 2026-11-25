/*
 * gdt.c — Global Descriptor Table
 *
 * Sets up TanoojOS's own GDT and loads it into the CPU.
 * Right now we run on GRUB's temporary GDT — this replaces it
 * with one we fully control.
 *
 * Access byte breakdown (for reference):
 *   Bit 7   : Present (must be 1 for valid segment)
 *   Bits 6-5: Privilege ring (0 = kernel, 3 = user)
 *   Bit 4   : Descriptor type (1 = code/data, 0 = system)
 *   Bit 3   : Executable (1 = code segment, 0 = data)
 *   Bit 2   : Direction/conforming
 *   Bit 1   : Read/write
 *   Bit 0   : Accessed (CPU sets this, we leave it 0)
 */

#include "gdt.h"
#include "terminal.h"

#define GDT_ENTRIES 3

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;

/* Declared in gdt_flush.asm — loads GDT and reloads segment registers */
extern void gdt_flush(uint32_t gdt_ptr_addr);

static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity)
{
    gdt[index].base_low    = (base & 0xFFFF);
    gdt[index].base_mid    = (base >> 16) & 0xFF;
    gdt[index].base_high   = (base >> 24) & 0xFF;

    gdt[index].limit_low   = (limit & 0xFFFF);
    gdt[index].granularity = ((limit >> 16) & 0x0F) | (granularity & 0xF0);

    gdt[index].access      = access;
}

void gdt_init(void) {
    gdt_ptr.limit = (sizeof(gdt_entry_t) * GDT_ENTRIES) - 1;
    gdt_ptr.base  = (uint32_t)&gdt;

    /* Entry 0: Null descriptor — required by CPU, all zeros */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Entry 1: Kernel code segment
     *   Base=0, Limit=4GiB, Ring 0, executable, readable
     *   Access : 0x9A = 1001 1010
     *   Granularity: 0xCF = page granularity, 32-bit */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Entry 2: Kernel data segment
     *   Base=0, Limit=4GiB, Ring 0, writable
     *   Access : 0x92 = 1001 0010 */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* Load GDT and reload segment registers */
    gdt_flush((uint32_t)&gdt_ptr);

}
