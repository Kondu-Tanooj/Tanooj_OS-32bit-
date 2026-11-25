# TanoojOS — A minimal x86 operating system

A bootable 32-bit OS written from scratch in x86 assembly and C.
Boots on real hardware via USB. No Linux, no existing OS beneath it.

## Project layout

```
myos/
├── boot/
│   └── boot.asm        — Multiboot header + assembly entry point (_start)
├── kernel/
│   ├── kernel.c        — kernel_main(): first C code that runs
│   └── terminal.c      — VGA text-mode driver (writes directly to 0xB8000)
├── include/
│   └── terminal.h      — Terminal API header
├── linker.ld           — Memory layout: kernel loads at 1 MiB
├── Makefile            — Build, run, ISO, USB flash targets
└── README.md           — This file
```

## What happens when you boot

```
Power on
  └─> BIOS/UEFI runs POST, finds bootable device
       └─> GRUB reads grub.cfg, finds 'multiboot /boot/tanooj.elf'
            └─> GRUB verifies Multiboot header in boot.asm
                 └─> GRUB switches CPU to 32-bit protected mode
                      └─> GRUB jumps to _start in boot.asm
                           └─> boot.asm sets up a 16 KiB stack
                                └─> Calls kernel_main() in kernel.c
                                     └─> TanoojOS RUNS
```

## Prerequisites

### Ubuntu / Debian / WSL2
```bash
sudo apt update
sudo apt install nasm gcc-multilib grub-pc-bin grub-common xorriso qemu-system-x86 gdb
```

### macOS (requires cross-compiler)
```bash
brew install nasm xorriso qemu
# Install i686-elf cross-compiler:
# https://wiki.osdev.org/GCC_Cross-Compiler
# Then edit Makefile: CC=i686-elf-gcc, LD=i686-elf-ld
```

## Quick start

```bash
# Build TanoojOS
make

# Boot in QEMU
make run

# Build a bootable ISO
make iso

# Test the ISO in QEMU
qemu-system-i386 -cdrom build/tanooj.iso

# Flash to USB and boot on real hardware
make usb DEVICE=/dev/sdb
```

## What you'll see

A cyan ASCII art "TanoojOS" banner followed by:
- ASCII art banner
- Multiboot magic number verification (proves GRUB loaded us correctly)
- Available RAM (read from the Multiboot info struct)
- "Kernel initialised successfully."

## Phase 2: What to build next

### 1. Global Descriptor Table (GDT)
The GDT defines memory segments and privilege levels (ring 0 = kernel, ring 3 = user).
Even though we mostly use paging, the GDT must be set up for the CPU to function properly.

```c
// Add to kernel/gdt.c
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

void gdt_init(void);   // Set up and load the GDT
```

### 2. Interrupt Descriptor Table (IDT) + PIC
Lets the CPU handle hardware interrupts (keyboard, timer) and exceptions (page fault, divide-by-zero).

```c
// Add to kernel/idt.c
void idt_init(void);
void pic_remap(int offset1, int offset2);
void irq_install_handler(int irq, void (*handler)(void));
```

### 3. Keyboard driver
Once you have the IDT, IRQ1 fires on every keypress.
Read the scancode from port 0x60 and convert to ASCII.

```c
// kernel/keyboard.c
#include <sys/io.h>   // inb() to read from I/O port
void keyboard_handler(void);
char keyboard_getchar(void);
```

### 4. Physical memory manager
Parse the Multiboot memory map to find free RAM.
Build a bitmap or stack allocator to hand out 4 KiB pages.

```c
// kernel/pmm.c
void  pmm_init(multiboot_info_t *mbi);
void *pmm_alloc_page(void);
void  pmm_free_page(void *addr);
```

### 5. Virtual memory / paging
Set up a page directory and page tables.
Map virtual addresses to physical addresses.
Enable the paging bit in CR0.

### 6. Heap allocator (kmalloc/kfree)
Build a simple heap on top of the physical memory manager.
Now your kernel can use dynamic memory.

### 7. Shell
Read keyboard input, parse commands, execute built-in commands.
This is when the OS starts to feel real.

## Useful resources

- **OSDev Wiki**: https://wiki.osdev.org — the definitive reference
- **OSDev Bare Bones**: https://wiki.osdev.org/Bare_Bones — official tutorial this project is based on
- **James Molloy's kernel tutorial**: http://www.jamesmolloy.co.uk/tutorial_html/
- **Writing an OS in Rust** (concepts apply to C too): https://os.phil-opp.com
- **Intel x86 manuals**: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- **SerenityOS** (inspiration, real hobby OS): https://github.com/SerenityOS/serenity

## Debugging tips

```bash
# Run with QEMU monitor to inspect registers
make run    # then press Ctrl-Alt-2 to enter QEMU monitor
(qemu) info registers
(qemu) x/10i $eip    # disassemble 10 instructions at EIP

# GDB debugging (two terminals)
make debug                          # starts QEMU paused, GDB server on :1234
# In another terminal:
gdb build/myos.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

## Booting on a real laptop

1. `make iso` to create `build/myos.iso`
2. Find your USB device: `lsblk` (look for your USB drive, e.g. `/dev/sdb`)
3. `make usb DEVICE=/dev/sdb` (will ask you to confirm)
4. Set laptop BIOS to boot from USB (usually F12 or F2 on startup)
5. Select "MyOS" from the GRUB menu
6. You'll see the green terminal on your laptop!

Most UEFI laptops need "Legacy Boot" or "CSM" enabled to boot 32-bit GRUB.
For UEFI-native booting, you'd need to add a UEFI stub — a Phase 7 project.
