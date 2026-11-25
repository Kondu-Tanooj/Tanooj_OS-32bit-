/*
 * shell.c — TanoojOS command-line shell
 *
 * The shell collects characters from the keyboard into a line buffer.
 * When Enter is pressed, it tokenises the line (splits on spaces),
 * matches the first token against the command table, and calls the
 * handler. Output goes to the terminal (rows 0-21). The input bar
 * (row 23) is managed by keyboard.c — the shell never touches it.
 *
 * Adding a new command:
 *   1. Write a handler: static void cmd_foo(int argc, char **argv)
 *   2. Add an entry to the commands[] table at the bottom
 *   That's it.
 */

#include "shell.h"
#include "terminal.h"
#include "pmm.h"
#include "heap.h"
#include "vfs.h"
#include "keyboard.h"
#include "editor.h"
#include "process.h"
#include "mount.h"
#include "auth.h"

/* from calc.c */
void calc_eval(const char *expr);
#include "tfs.h"
#include "ata.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   Line buffer
   ----------------------------------------------------------------------- */
#define LINE_MAX  256
#define ARG_MAX   16

static char line_buf[LINE_MAX];
static int  line_len = 0;
static int  passwd_pending = 0;  /* 0=off 1=new pass 2=confirm */
static char passwd_first[64];

/* -----------------------------------------------------------------------
   String helpers (no libc)
   ----------------------------------------------------------------------- */
static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* -----------------------------------------------------------------------
   Tokeniser — split line_buf into argv on spaces, return argc
   Modifies line_buf in-place (replaces spaces with '\0').
   ----------------------------------------------------------------------- */
static int tokenise(char *argv[], int max_args) {
    int argc = 0;
    char *p  = line_buf;

    while (*p) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Start of a token */
        if (argc < max_args) argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = '\0'; p++; }
    }
    return argc;
}

/* -----------------------------------------------------------------------
   Built-in command handlers
   ----------------------------------------------------------------------- */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  TanoojOS v0.1 -- command reference");
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("  --------------------------------------------");

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Shell");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    help              show this list");
    terminal_writeline("    clear / cls       clear the screen");
    terminal_writeline("    echo [text]       print text to screen");
    terminal_writeline("    color [0-15]      change text colour");
    terminal_writeline("    repeat [n] [txt]  print text n times");

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  System");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    uptime            how long OS has been running");
    terminal_writeline("    whoami            show current user");
    terminal_writeline("    uname             show OS info");
    terminal_writeline("    reboot            restart the system");
    terminal_writeline("    shutdown          power off");

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Files");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    ls [dir]          list directory contents");
    terminal_writeline("    cat <file>        show file contents");
    terminal_writeline("    write <f> <text>  write text to file");
    terminal_writeline("    mkdir <dir>       create directory");
    terminal_writeline("    rm <file>         delete file or empty dir");
    terminal_writeline("    tree              show full filesystem tree");

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Navigation");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    cd <dir>          change directory");
    terminal_writeline("    pwd               show current path");
    terminal_writeline("    mem               show RAM and heap stats");

    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("  --------------------------------------------");
    terminal_writeline("  Up/Down=history  Tab=complete  PgUp/Dn=scroll");
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Disk (persistent)");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    save <f>          save file to disk");
    terminal_writeline("    load <f>          load file from disk");
    terminal_writeline("    dsave             save all files to disk");
    terminal_writeline("    dls               list files on disk");
    terminal_writeline("    drm <f>           delete file from disk");
    terminal_writeline("    df                show disk info");
    terminal_writeline("    edit [file]       open text editor");
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Processes");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    ps                list all running tasks");
    terminal_writeline("    kill <pid>        kill a task by pid");
    terminal_writeline("    spawn [name]      create a background task");
    terminal_writeline("    yield             give up CPU to next task");
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Disks");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    mount             list all mounted drives");
    terminal_writeline("    use <point>       switch active disk (/disk2)");
    terminal_writeline("    unmount <point>   unmount a disk");
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Security");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    passwd            change boot password");
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Tools");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("    calc <expr>       calculator  e.g. calc 2+3*4");
    terminal_writeline("    Ctrl++  zoom in   Ctrl+-  zoom out   Ctrl+0  reset");
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_init();
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  Screen cleared. Type 'help' for commands.");
}

static void cmd_echo(int argc, char **argv) {
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("  ");
    for (int i = 1; i < argc; i++) {
        terminal_write(argv[i]);
        if (i < argc - 1) terminal_putchar(' ');
    }
    terminal_putchar('\n');
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  Memory report:");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);

    uint32_t total = pmm_get_total_pages();
    uint32_t free  = pmm_get_free_pages();
    uint32_t used  = total - free;

    terminal_write("    Total RAM : ");
    terminal_write_dec(total * 4);
    terminal_writeline(" KiB");

    terminal_write("    Used      : ");
    terminal_write_dec(used * 4);
    terminal_writeline(" KiB");

    terminal_write("    Free      : ");
    terminal_write_dec(free * 4);
    terminal_writeline(" KiB");

    terminal_writeline("");
    heap_print_stats();
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  ----------------------------------------");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("  TanoojOS v0.1");
    terminal_writeline("  A minimal 32-bit x86 OS built from scratch.");
    terminal_writeline("");
    terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    terminal_writeline("  Phases complete:");
    terminal_writeline("    Phase 1 - Bootloader + VGA terminal");
    terminal_writeline("    Phase 2 - GDT + IDT + Keyboard driver");
    terminal_writeline("    Phase 3 - Physical memory + heap allocator");
    terminal_writeline("    Phase 4 - Command line shell  <-- you are here");
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  ----------------------------------------");
}

static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Clear screen and show shutdown message */
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    for (int i = 0; i < 80*25; i++)
        *(volatile uint16_t*)(0xB8000 + i*2) = 0x0720;

    /* Print message directly to VGA */
    const char *msg1 = "  TanoojOS is shutting down...";
    const char *msg2 = "  It is now safe to power off your computer.";
    const char *msg3 = "  (Press power button to turn off)";
    for (int i=0; msg1[i]; i++) *(volatile uint16_t*)(0xB8000+i*2)=(uint16_t)(unsigned char)msg1[i]|0x0F00;
    for (int i=0; msg2[i]; i++) *(volatile uint16_t*)(0xB8000+160+i*2)=(uint16_t)(unsigned char)msg2[i]|0x0700;
    for (int i=0; msg3[i]; i++) *(volatile uint16_t*)(0xB8000+320+i*2)=(uint16_t)(unsigned char)msg3[i]|0x0800;

    __asm__ volatile ("cli");

    /* Method 1: ACPI shutdown via SCI — works on most real hardware */
    /* First try to find ACPI by sending to common ports */
    __asm__ volatile ("movw $0x604, %dx; movw $0x2000, %ax; outw %ax, %dx");
    for (volatile int d=0; d<1000000; d++);

    /* Method 2: APM shutdown */
    __asm__ volatile ("movw $0xB004, %dx; movw $0x2000, %ax; outw %ax, %dx");
    for (volatile int d=0; d<1000000; d++);

    /* Method 3: Try port 0x4004 (some chipsets) */
    __asm__ volatile ("movw $0x4004, %dx; movw $0x3400, %ax; outw %ax, %dx");
    for (volatile int d=0; d<1000000; d++);

    /* If nothing worked — just halt cleanly */
    /* Screen already shows "safe to power off" message */
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    terminal_writeline("  Rebooting TanoojOS...");
    __asm__ volatile ("cli");
    /* Keyboard controller reset — works on real hardware and QEMU */
    uint8_t val;
    do { __asm__ volatile ("inb $0x64, %0" : "=a"(val)); } while (val & 0x02);
    __asm__ volatile ("outb %0, $0x64" :: "a"((uint8_t)0xFE));
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_color(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: color [0-15]");
        return;
    }
    /* Parse the number */
    int n = 0;
    for (int i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
        n = n * 10 + (argv[1][i] - '0');
    if (n < 0 || n > 15) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Color must be 0-15");
        return;
    }
    terminal_set_color((vga_color_t)n, COLOR_BLACK);
    terminal_write("  Color set to ");
    terminal_write_dec((uint32_t)n);
    terminal_writeline(". Type 'color 10' to go back to green.");
}

static void cmd_repeat(int argc, char **argv) {
    if (argc < 3) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: repeat [count] [text]");
        return;
    }
    int n = 0;
    for (int i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
        n = n * 10 + (argv[1][i] - '0');
    if (n > 100) n = 100; /* safety cap */
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    for (int i = 0; i < n; i++) {
        terminal_write("  ");
        terminal_writeline(argv[2]);
    }
}

/* -----------------------------------------------------------------------
   Filesystem commands
   ----------------------------------------------------------------------- */

static void cmd_ls(int argc, char **argv) {
    vfs_node_t *dir = NULL;
    if (argc > 1)
        dir = vfs_resolve(argv[1]);
    else
        dir = vfs_get_cwd();

    if (!dir) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  ls: directory not found");
        return;
    }
    if (dir->type != VFS_DIR) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  ls: not a directory");
        return;
    }

    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_write("  Contents of /");
    terminal_writeline(dir->name);
    terminal_writeline("");

    if (dir->child_count == 0) {
        terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
        terminal_writeline("  (empty)");
        return;
    }

    for (int i = 0; i < dir->child_count; i++) {
        vfs_node_t *child = dir->children[i];
        terminal_write("  ");
        if (child->type == VFS_DIR) {
            terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            terminal_write(child->name);
            terminal_write("/");
        } else {
            terminal_set_color(COLOR_WHITE, COLOR_BLACK);
            terminal_write(child->name);
            terminal_write("  (");
            terminal_write_dec(child->size);
            terminal_write(" B)");
        }
        terminal_writeline("");
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: cat <filename>");
        return;
    }
    vfs_node_t *file = vfs_resolve(argv[1]);
    if (!file) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  cat: file not found: ");
        terminal_writeline(argv[1]);
        return;
    }
    if (file->type == VFS_DIR) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  cat: is a directory");
        return;
    }
    if (!file->data || file->size == 0) {
        terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
        terminal_writeline("  (empty file)");
        return;
    }
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("  ");
    terminal_write(file->data);
}

static void cmd_write(int argc, char **argv) {
    if (argc < 3) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: write <filename> <content>");
        return;
    }
    vfs_node_t *cwd  = vfs_get_cwd();
    vfs_node_t *file = vfs_find(cwd, argv[1]);
    if (!file) {
        file = vfs_create(cwd, argv[1]);
        if (!file) {
            terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            terminal_writeline("  write: could not create file");
            return;
        }
    }
    if (file->type == VFS_DIR) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  write: is a directory");
        return;
    }
    /* Calculate length first, then pass it to vfs_write */
    uint32_t len = 0;
    while (argv[2][len]) len++;
    vfs_write(file, argv[2], len);
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_write("  Written ");
    terminal_write_dec(file->size);
    terminal_write(" bytes to ");
    terminal_writeline(argv[1]);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: mkdir <dirname>");
        return;
    }
    vfs_node_t *dir = vfs_mkdir(vfs_get_cwd(), argv[1]);
    if (!dir) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  mkdir: failed (already exists or no space)");
        return;
    }
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_write("  Created directory: ");
    terminal_writeline(argv[1]);
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        vfs_set_cwd(vfs_get_root());
        return;
    }
    vfs_node_t *dir = vfs_resolve(argv[1]);
    if (!dir) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  cd: not found: ");
        terminal_writeline(argv[1]);
        return;
    }
    if (dir->type != VFS_DIR) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  cd: not a directory");
        return;
    }
    vfs_set_cwd(dir);
    keyboard_redraw_prompt(); /* update path in prompt immediately */
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Walk up to root building path */
    vfs_node_t *node = vfs_get_cwd();
    char path[256];
    int pos = 255;
    path[pos] = 0;

    while (node && node != node->parent) {
        int len = 0; while (node->name[len]) len++;
        pos -= len;
        if (pos < 1) break;
        for (int i = 0; i < len; i++) path[pos + i] = node->name[i];
        path[--pos] = '/';
        node = node->parent;
    }
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("  ");
    terminal_writeline(&path[pos]);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: rm <filename>");
        return;
    }
    vfs_node_t *file = vfs_resolve(argv[1]);
    if (!file) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  rm: not found: ");
        terminal_writeline(argv[1]);
        return;
    }
    int result = vfs_delete(file);
    if (result == -2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  rm: directory is not empty");
        return;
    }
    if (result != 0) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  rm: cannot delete");
        return;
    }
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_write("  Deleted: ");
    terminal_writeline(argv[1]);
}

static void cmd_tree(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    terminal_writeline("  Filesystem tree:");
    vfs_print_tree(vfs_get_root(), 1);
}

/* -----------------------------------------------------------------------
   Disk commands
   ----------------------------------------------------------------------- */

static void cmd_save(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: save <filename>");
        terminal_writeline("  Saves a file from the in-memory filesystem to disk.");
        return;
    }
    if (!tfs_is_mounted()) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  No disk mounted.");
        return;
    }
    vfs_node_t *file = vfs_resolve(argv[1]);
    if (!file || file->type != VFS_FILE) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  File not found: ");
        terminal_writeline(argv[1]);
        return;
    }
    const char *data = file->data ? file->data : "";
    int r = tfs_save_file(file->name, data, file->size);
    if (r == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  Saved to disk: ");
        terminal_write(file->name);
        terminal_write("  (");
        terminal_write_dec(file->size);
        terminal_writeline(" B)");
    } else {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Save failed.");
    }
}

static void cmd_load(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: load <filename>");
        terminal_writeline("  Loads a file from disk into the in-memory filesystem.");
        return;
    }
    if (!tfs_is_mounted()) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  No disk mounted.");
        return;
    }
    char *buf = (char *)kmalloc(32768);
    if (!buf) { terminal_writeline("  Out of memory."); return; }

    int r = tfs_load_file(argv[1], buf, 32767);
    if (r < 0) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Not found on disk: ");
        terminal_writeline(argv[1]);
        kfree(buf);
        return;
    }

    /* Write into VFS */
    vfs_node_t *file = vfs_find(vfs_get_cwd(), argv[1]);
    if (!file) file = vfs_create(vfs_get_cwd(), argv[1]);
    if (file) {
        vfs_write(file, buf, (uint32_t)r);
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  Loaded: ");
        terminal_write(argv[1]);
        terminal_write("  (");
        terminal_write_dec((uint32_t)r);
        terminal_writeline(" B)");
    }
    kfree(buf);
}

static void cmd_dsave(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!tfs_is_mounted()) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  No disk mounted.");
        return;
    }
    /* Save all files in current directory to disk */
    vfs_node_t *dir = vfs_get_cwd();
    int saved = 0;
    for (int i = 0; i < dir->child_count; i++) {
        vfs_node_t *f = dir->children[i];
        if (f->type != VFS_FILE) continue;
        const char *data = f->data ? f->data : "";
        if (tfs_save_file(f->name, data, f->size) == 0) {
            terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            terminal_write("  Saved: "); terminal_writeline(f->name);
            saved++;
        }
    }
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("  Total saved: "); terminal_write_dec((uint32_t)saved);
    terminal_writeline(" file(s)");
}

static void cmd_dls(int argc, char **argv) {
    (void)argc; (void)argv;
    tfs_list();
}

static void cmd_drm(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: drm <filename>");
        return;
    }
    if (tfs_delete_file(argv[1]) == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  Deleted from disk: "); terminal_writeline(argv[1]);
    } else {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Not found on disk: "); terminal_writeline(argv[1]);
    }
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!tfs_is_mounted()) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  No disk mounted.");
        return;
    }
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  Disk info (TanoojFS):");
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("    Filesystem : TanoojFS v1");
    terminal_writeline("");
    terminal_write("    Total      : ");
    terminal_write_dec(TFS_DISK_SECTS * SECTOR_SIZE / 1024);
    terminal_writeline(" KiB");
    terminal_write("    Files      : ");
    terminal_write_dec((uint32_t)tfs_file_count());
    terminal_write(" / ");
    terminal_write_dec(TFS_MAX_FILES);
    terminal_writeline("");
}

static void cmd_paging(int argc, char **argv) {
    (void)argc; (void)argv;
    paging_print_stats();
    terminal_writeline("");
    pmm_print_stats();
    terminal_writeline("");
    heap_print_stats();
}

static void cmd_edit(int argc, char **argv) {
    const char *fname = (argc > 1) ? argv[1] : NULL;
    editor_open(fname);
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    process_list();
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_write("  Total tasks: ");
    terminal_write_dec((uint32_t)process_count());
    terminal_writeline("");
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: kill <pid>");
        return;
    }
    uint32_t pid = 0;
    for (int i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
        pid = pid * 10 + (uint32_t)(argv[1][i] - '0');
    if (pid <= 1) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Cannot kill kernel task (pid 1).");
        return;
    }
    process_kill(pid);
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_write("  Killed pid ");
    terminal_write_dec(pid);
    terminal_writeline("");
}

/* -----------------------------------------------------------------------
   Built-in background tasks
   ----------------------------------------------------------------------- */
static void task_clock(void) {
    extern volatile uint32_t timer_ticks;
    while (1) {
        process_sleep(18 * 5);
        uint32_t s = timer_ticks / 18;
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_write("  [clock] uptime: ");
        terminal_write_dec(s);
        terminal_writeline("s");
    }
}
static void task_memwatch(void) {
    while (1) {
        process_sleep(18 * 10);
        uint32_t free_kb = pmm_get_free_pages() * 4;
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_write("  [memwatch] free RAM: ");
        terminal_write_dec(free_kb);
        terminal_writeline(" KiB");
    }
}
static void task_counter(void) {
    uint32_t n = 0;
    while (1) {
        process_sleep(18 * 3);
        n++;
        terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
        terminal_write("  [counter] tick: ");
        terminal_write_dec(n);
        terminal_writeline("");
    }
}
static void task_idle(void) {
    while (1) process_sleep(18 * 60);
}
typedef struct { const char *name; task_fn_t fn; const char *desc; } task_def_t;
static const task_def_t task_defs[] = {
    { "clock",    task_clock,    "print uptime every 5s"    },
    { "memwatch", task_memwatch, "print free RAM every 10s" },
    { "counter",  task_counter,  "count every 3s"           },
    { "idle",     task_idle,     "background idle task"     },
    { NULL, NULL, NULL }
};

static void cmd_yield(int argc, char **argv) {
    (void)argc; (void)argv;
    process_yield();
}

static void cmd_spawn(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        terminal_writeline("  Available tasks:");
        terminal_set_color(COLOR_WHITE, COLOR_BLACK);
        for (int i = 0; task_defs[i].name; i++) {
            terminal_write("    spawn ");
            terminal_write(task_defs[i].name);
            terminal_write("  -- ");
            terminal_writeline(task_defs[i].desc);
        }
        return;
    }
    for (int i = 0; task_defs[i].name; i++) {
        const char *a=argv[1], *b=task_defs[i].name;
        int eq=1; while(*a||*b){if(*a!=*b){eq=0;break;}a++;b++;}
        if (eq) {
            task_t *t = process_create(task_defs[i].name, task_defs[i].fn);
            if (t) {
                terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
                terminal_write("  Spawned '");
                terminal_write(task_defs[i].name);
                terminal_write("' pid=");
                terminal_write_dec(t->pid);
                terminal_write("  -- ");
                terminal_writeline(task_defs[i].desc);
            }
            return;
        }
    }
    terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    terminal_write("  Unknown task: ");
    terminal_writeline(argv[1]);
    terminal_writeline("  Type 'spawn' to see available tasks.");
}

static void cmd_mount(int argc, char **argv) {
    (void)argc; (void)argv;
    mount_list();
}

static void cmd_use(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: use <mountpoint>  e.g. use /disk2");
        return;
    }
    if (mount_select(argv[1]) == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  Switched to ");
        terminal_writeline(argv[1]);
        terminal_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
        terminal_writeline("  Disk commands (dls, save, load) now use this drive.");
    } else {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Mount point not found: ");
        terminal_writeline(argv[1]);
    }
}

static void cmd_unmount(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  Usage: unmount <mountpoint>  e.g. unmount /disk2");
        return;
    }
    if (unmount_drive(argv[1]) == 0) {
        terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        terminal_write("  Unmounted: ");
        terminal_writeline(argv[1]);
    } else {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Cannot unmount: ");
        terminal_writeline(argv[1]);
    }
}

static void cmd_passwd(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("  Enter new boot password:");
    terminal_write("  > ");
    passwd_pending = 1;
}

/* Called from execute() before normal command dispatch */
static int passwd_intercept(const char *line) {
    if (!passwd_pending) return 0;

    if (passwd_pending == 1) {
        /* Stage 1 — got new password, ask to confirm */
        if (line[0] == 0) {
            terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            terminal_writeline("  Password cannot be empty. Cancelled.");
            passwd_pending = 0;
            return 1;
        }
        /* Save it and ask for confirm */
        int i = 0;
        while (line[i] && i < 63) { passwd_first[i] = line[i]; i++; }
        passwd_first[i] = 0;
        passwd_pending = 2;
        terminal_set_color(COLOR_WHITE, COLOR_BLACK);
        terminal_writeline("  Confirm new password:");
        terminal_write("  > ");
        return 1;
    }

    if (passwd_pending == 2) {
        /* Stage 2 — confirm password */
        passwd_pending = 0;
        /* Compare passwd_first with line */
        int match = 1;
        int i = 0;
        while (passwd_first[i] || line[i]) {
            if (passwd_first[i] != line[i]) { match = 0; break; }
            i++;
        }
        if (!match) {
            terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            terminal_writeline("  Passwords do not match. Cancelled.");
            return 1;
        }
        auth_set_password(passwd_first);
        return 1;
    }
    return 0;
}

static void cmd_calc(int argc, char **argv) {
    if (argc < 2) {
        terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        terminal_writeline("  TanoojCalc — expression calculator");
        terminal_set_color(COLOR_WHITE, COLOR_BLACK);
        terminal_writeline("  Usage: calc <expression>");
        terminal_writeline("  Examples:");
        terminal_writeline("    calc 2 + 2");
        terminal_writeline("    calc 10 * 5 - 3");
        terminal_writeline("    calc (100 / 4) + 25");
        terminal_writeline("    calc 2 ^ 8");
        terminal_writeline("    calc -5 * (3 + 2)");
        return;
    }
    /* Join all args into one expression string */
    static char expr[256];
    int pos = 0;
    for (int i = 1; i < argc && pos < 254; i++) {
        for (int j = 0; argv[i][j] && pos < 254; j++)
            expr[pos++] = argv[i][j];
        if (i < argc - 1) expr[pos++] = ' ';
    }
    expr[pos] = 0;
    calc_eval(expr);
}

/* -----------------------------------------------------------------------
   Extra commands
   ----------------------------------------------------------------------- */

static void cmd_cls(int argc, char **argv) { cmd_clear(argc, argv); }

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    /* timer_ticks is in idt.c — expose via extern */
    extern volatile uint32_t timer_ticks;
    uint32_t secs  = timer_ticks / 18;
    uint32_t mins  = secs / 60;
    uint32_t hours = mins / 60;
    secs %= 60; mins %= 60;
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_write("  Uptime: ");
    if (hours) { terminal_write_dec(hours); terminal_write("h "); }
    if (mins)  { terminal_write_dec(mins);  terminal_write("m "); }
    terminal_write_dec(secs);
    terminal_writeline("s");
}

static void cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("  tanooj");
}

static void cmd_uname(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_set_color(COLOR_WHITE, COLOR_BLACK);
    terminal_writeline("  TanoojOS v0.1  x86 32-bit  built from scratch");
}

/* -----------------------------------------------------------------------
   Command table
   ----------------------------------------------------------------------- */
typedef struct {
    const char *name;
    void (*handler)(int argc, char **argv);
    const char *desc;
} command_t;

static const command_t commands[] = {
    { "help",   cmd_help,   "show this list"          },
    { "clear",  cmd_clear,  "clear the screen"        },
    { "echo",   cmd_echo,   "print text"              },
    { "mem",    cmd_mem,    "memory stats"            },
    { "about",  cmd_about,  "about TanoojOS"          },
    { "reboot",   cmd_reboot,   "restart the system"      },
    { "shutdown", cmd_shutdown, "power off the system"    },
    { "color",  cmd_color,  "change text color [0-15]"},
    { "repeat", cmd_repeat, "repeat [n] [text]"       },
    { "ls",     cmd_ls,     "list directory"           },
    { "cat",    cmd_cat,    "show file contents"       },
    { "write",  cmd_write,  "write [file] [content]"  },
    { "mkdir",  cmd_mkdir,  "make directory"           },
    { "cd",     cmd_cd,     "change directory"         },
    { "pwd",    cmd_pwd,    "show current directory"   },
    { "rm",     cmd_rm,     "delete file or dir"       },
    { "tree",     cmd_tree,     "show filesystem tree"       },
    { "cls",      cmd_cls,      "clear screen (alias)"       },
    { "uptime",   cmd_uptime,   "show system uptime"         },
    { "whoami",   cmd_whoami,   "show current user"          },
    { "uname",    cmd_uname,    "show system info"           },
    { "save",     cmd_save,     "save file to disk"          },
    { "load",     cmd_load,     "load file from disk"        },
    { "dsave",    cmd_dsave,    "save all files to disk"     },
    { "dls",      cmd_dls,      "list disk files"            },
    { "drm",      cmd_drm,      "delete file from disk"      },
    { "df",       cmd_df,       "show disk info"             },
    { "edit",     cmd_edit,     "edit [file] — open editor"  },
    { "ps",       cmd_ps,       "list all tasks"             },
    { "kill",     cmd_kill,     "kill <pid>"                 },
    { "spawn",    cmd_spawn,    "spawn [name] background task"},
    { "mount",    cmd_mount,    "list mounted drives"         },
    { "use",      cmd_use,      "use <mountpoint>  switch disk"},
    { "unmount",  cmd_unmount,  "unmount <mountpoint>"        },
    { "passwd",   cmd_passwd,   "change boot password"        },
    { "calc",     cmd_calc,     "calc <expr>  e.g. calc 2+3"  },
    { "yield",    cmd_yield,    "yield CPU to next task"     },
    { "paging",   cmd_paging,   "show paging/memory info"    },
};
#define NUM_COMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

/* -----------------------------------------------------------------------
   Execute a line
   ----------------------------------------------------------------------- */
static void execute(void) {
    /* Check if waiting for password input */
    if (passwd_intercept(line_buf)) return;

    char *argv[ARG_MAX];
    int   argc = tokenise(argv, ARG_MAX);

    if (argc == 0) return; /* empty line */

    /* Print the command to terminal so you can see what you ran */
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_write("  $ ");
    /* Reconstruct the original command from argv */
    for (int i = 0; i < argc; i++) {
        terminal_write(argv[i]);
        if (i < argc - 1) terminal_putchar(' ');
    }
    terminal_putchar('\n');

    /* Match against command table */
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (str_eq(argv[0], commands[i].name)) {
            commands[i].handler(argc, argv);
            terminal_putchar('\n');
            return;
        }
    }

    /* Unknown command */
    terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    terminal_write("  Unknown command: '");
    terminal_write(argv[0]);
    terminal_writeline("'  (type 'help' for a list)");
    terminal_putchar('\n');
}

/* -----------------------------------------------------------------------
   shell_feed — called by keyboard.c for every character
   ----------------------------------------------------------------------- */
void shell_feed(char c) {
    if (c == '\n') {
        /* Null-terminate and execute */
        line_buf[line_len] = '\0';
        execute();
        line_len = 0;
        return;
    }
    if (c == '\b') {
        if (line_len > 0) line_len--;
        return;
    }
    if (line_len < LINE_MAX - 1) {
        line_buf[line_len++] = c;
    }
}

/* -----------------------------------------------------------------------
   shell_init — print welcome message
   ----------------------------------------------------------------------- */
void shell_init(void) {
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  +------------------------------------+");
    terminal_writeline("  |    Welcome to TanoojOS Shell      |");
    terminal_writeline("  |    Type 'help' to get started     |");
    terminal_writeline("  +------------------------------------+");
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("");
}
