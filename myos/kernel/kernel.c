/*
 * kernel.c — TanoojOS kernel entry point
 */

#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "pmm.h"
#include "heap.h"
#include "shell.h"
#include "vfs.h"
#include "ata.h"
#include "tfs.h"
#include "paging.h"
#include "process.h"
#include "mount.h"
#include "auth.h"
#include "mouse.h"
#include <stdint.h>
#include <stddef.h>

#define MULTIBOOT_MAGIC 0x2BADB002

static void print_banner(void) {
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  _____                      _ ___  ____  ");
    terminal_writeline(" |_   _|_ _ _ _  ___  ___  (_) _ \\/ ___| ");
    terminal_writeline("   | |/ _` | '  \\/ _ \\/ _ \\ | | | \\___ \\ ");
    terminal_writeline("   | | (_| | | | (_) | (_) || | |_|___) |");
    terminal_writeline("   |_|\\__,_|_| |_\\___/\\___(_)_|\\___/____/ ");
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("  TanoojOS v0.1 -- built from scratch\n");
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    /* Initialise only what auth needs — GDT/IDT for stability, ATA for disk */
    terminal_init();
    gdt_init();
    idt_init();

    /* Boot password FIRST — before any output or init messages */
    pmm_init(mbi, 0);
    heap_init();
    mount_init(); /* need ATA ready for password hash */
    auth_boot();  /* halts if wrong password */

    /* Auth passed — now do full init and show banner */
    keyboard_init();
    paging_init();
    print_banner();
    vfs_init();
    mouse_init();

    /* Warn if not booted by GRUB */
    if (magic != MULTIBOOT_MAGIC) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Warning: invalid bootloader magic.");
    }

    /* Phase 8: multitasking */
    process_init();

    /* Hand off to shell */
    shell_init();

    for (;;) __asm__ volatile ("hlt");
}
