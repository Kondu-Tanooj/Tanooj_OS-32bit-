/*
 * linker.ld — Kernel linker script
 *
 * This tells the linker the EXACT memory layout of the kernel binary.
 *
 * The CPU starts in "real mode" (16-bit, 1 MiB limit) then GRUB switches
 * us to 32-bit protected mode. We load the kernel at the 1 MiB mark (0x100000)
 * — below that is BIOS data and VGA buffers.
 *
 * Section layout:
 *   .multiboot  — GRUB header (must be in first 8 KiB searched)
 *   .text       — executable code
 *   .rodata     — read-only data (string literals, const arrays)
 *   .data       — initialised read-write data
 *   .bss        — uninitialised data (zeroed by convention; we zero it below)
 */

ENTRY(_start)

SECTIONS
{
    /* Load at 1 MiB — standard for multiboot kernels */
    . = 1M;

    /* .multiboot MUST come first so GRUB finds the header */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    /* Read-only data: string literals, vtables, etc. */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialised global/static variables */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /*
     * Uninitialised data + common symbols.
     * The C standard requires BSS to be zero-initialised.
     * GRUB doesn't zero it for us, so a real kernel would do this in boot.asm.
     * For now GCC's -nostdlib leaves it zeroed by the ELF loader in QEMU.
     */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Symbol marking the end of the kernel — used by pmm.c */
    _kernel_end = .;

    /* Discard debug sections to keep the binary small */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
    }
}
