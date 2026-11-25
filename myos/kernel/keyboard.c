/*
 * keyboard.c — PS/2 keyboard driver
 *
 * Screen layout:
 *   Rows  0–21 : terminal output (scrollable)
 *   Row   22   : separator line  "---..."
 *   Row   23   : input bar       "> typed text here"
 *   Row   24   : status bar      "TanoojOS  uptime: Ns  PgUp/Dn=scroll"
 *
 * Keyboard input goes directly to the input bar on row 23.
 * Terminal output (printk etc.) uses rows 0-21 only.
 * The two areas never overlap.
 */

#include "keyboard.h"
#include "idt.h"
#include "terminal.h"
#include "shell.h"
#include "mouse.h"
#include "editor.h"
#include "vfs.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
   Port I/O
   ----------------------------------------------------------------------- */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* -----------------------------------------------------------------------
   Direct VGA — for the fixed input bar and separator (bypasses terminal)
   ----------------------------------------------------------------------- */
#define VGA  ((volatile uint16_t *)0xB8000)
#define VGA_W 80

#define ROW_SEP    22   /* separator line */
#define ROW_INPUT  23   /* input bar */
#define INPUT_MAX   72  /* max typed chars (leaves room for path prompt) */

static char input_buf[INPUT_MAX + 1]; /* full input buffer               */
static int  input_len = 0;            /* number of chars in buffer       */
static int  input_cur = 0;            /* cursor position (0..input_len)  */

static void draw_separator(void) {
    for (int i = 0; i < VGA_W; i++)
        VGA[ROW_SEP * VGA_W + i] = (uint16_t)'-' | (0x08 << 8); /* dark grey */
}

/* Build the current path string into buf, return length */
static int get_cwd_path(char *buf, int max) {
    vfs_node_t *node = vfs_get_cwd();
    if (!node) { buf[0] = '/'; buf[1] = 0; return 1; }

    /* Walk up to root collecting names */
    char parts[8][32];
    int  depth = 0;
    while (node && node != node->parent && depth < 8) {
        int j = 0;
        while (node->name[j] && j < 31) { parts[depth][j] = node->name[j]; j++; }
        parts[depth][j] = 0;
        depth++;
        node = node->parent;
    }

    /* Build path from root down */
    int pos = 0;
    if (depth == 0) { buf[pos++] = '/'; buf[pos] = 0; return pos; }
    for (int i = depth - 1; i >= 0 && pos < max - 2; i--) {
        buf[pos++] = '/';
        for (int j = 0; parts[i][j] && pos < max - 1; j++)
            buf[pos++] = parts[i][j];
    }
    buf[pos] = 0;
    return pos;
}

static int prompt_len = 0; /* length of the prompt part (path + " > ") */

static void input_redraw(void) {
    /* Clear the whole row */
    for (int i = 0; i < VGA_W; i++)
        VGA[ROW_INPUT * VGA_W + i] = (uint16_t)' ' | (0x07 << 8);

    /* Build prompt: [/path] >  */
    char path[32];
    int  plen = get_cwd_path(path, 30);
    int  col  = 0;

    /* "[" in dark grey */
    VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)'[' | (0x08 << 8);

    /* path in cyan */
    for (int i = 0; i < plen && col < VGA_W - 4; i++)
        VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)(unsigned char)path[i] | (0x0B << 8);

    /* "] > " */
    VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)']' | (0x08 << 8);
    VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)' ' | (0x07 << 8);
    VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)'>' | (0x0A << 8);
    VGA[ROW_INPUT * VGA_W + col++] = (uint16_t)' ' | (0x07 << 8);

    prompt_len = col;

    /* Draw typed buffer */
    for (int i = 0; i < input_len && col < VGA_W - 1; i++)
        VGA[ROW_INPUT * VGA_W + col++] =
            (uint16_t)(unsigned char)input_buf[i] | (0x0F << 8);

    /* Draw cursor */
    int cur_col = prompt_len + input_cur;
    if (cur_col < VGA_W) {
        char cur_ch = (input_cur < input_len) ? input_buf[input_cur] : ' ';
        VGA[ROW_INPUT * VGA_W + cur_col] =
            (uint16_t)(unsigned char)cur_ch | (0x20 << 8); /* green bg */
    }
}

static void draw_input_prompt(void) {
    input_len = 0;
    input_cur = 0;
    input_redraw();
}

static void input_putchar(char c) {
    if (c == '\b') {
        if (input_cur > 0) {
            /* Remove char before cursor — shift rest left */
            input_cur--;
            for (int i = input_cur; i < input_len - 1; i++)
                input_buf[i] = input_buf[i + 1];
            input_len--;
            input_buf[input_len] = 0;
            input_redraw();
        }
        return;
    }
    if (c == '\n') {
        input_len = 0;
        input_cur = 0;
        input_redraw();
        return;
    }
    if (input_len < INPUT_MAX) {
        /* Insert char at cursor position — shift rest right */
        for (int i = input_len; i > input_cur; i--)
            input_buf[i] = input_buf[i - 1];
        input_buf[input_cur] = c;
        input_len++;
        input_cur++;
        input_buf[input_len] = 0;
        input_redraw();
    }
}

static void input_move_left(void)  { if (input_cur > 0)         { input_cur--; input_redraw(); } }
static void input_move_right(void) { if (input_cur < input_len) { input_cur++; input_redraw(); } }
static void input_move_home(void)  { input_cur = 0;         input_redraw(); }
static void input_move_end(void)   { input_cur = input_len; input_redraw(); }

/* -----------------------------------------------------------------------
   Scancode tables — US QWERTY, scancode set 1
   ----------------------------------------------------------------------- */
static const char scancode_ascii[128] = {
/*00*/  0,    0,  '1', '2', '3', '4', '5', '6',
/*08*/ '7',  '8','9',  '0', '-', '=', '\b','\t',
/*10*/ 'q',  'w','e',  'r', 't', 'y', 'u', 'i',
/*18*/ 'o',  'p','[',  ']','\n',  0,  'a', 's',
/*20*/ 'd',  'f','g',  'h', 'j', 'k', 'l', ';',
/*28*/'\'',  '`', 0,  '\\','z',  'x', 'c', 'v',
/*30*/ 'b',  'n','m',  ',', '.', '/',  0,  '*',
/*38*/  0,   ' ', 0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,  '7',
       '8',  '9','-',  '4', '5', '6', '+', '1',
       '2',  '3','0',  '.',  0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
};

static const char scancode_shift[128] = {
/*00*/  0,    0,  '!', '@', '#', '$', '%', '^',
/*08*/ '&',  '*','(',  ')', '_', '+', '\b','\t',
/*10*/ 'Q',  'W','E',  'R', 'T', 'Y', 'U', 'I',
/*18*/ 'O',  'P','{',  '}','\n',  0,  'A', 'S',
/*20*/ 'D',  'F','G',  'H', 'J', 'K', 'L', ':',
/*28*/ '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
/*30*/ 'B',  'N','M',  '<', '>', '?',  0,  '*',
/*38*/  0,   ' ', 0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
        0,    0,  0,    0,   0,   0,   0,   0,
};

/* -----------------------------------------------------------------------
   Command history
   ----------------------------------------------------------------------- */
#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][INPUT_MAX + 1];
static int  history_count = 0;
static int  history_head  = 0;
static int  history_pos   = -1;

static void history_up(void) {
    if (history_count == 0) return;
    if (history_pos < history_count - 1) history_pos++;
    else return;
    int idx = (history_head - 1 - history_pos + HISTORY_SIZE * 2) % HISTORY_SIZE;
    int len = 0; while (history[idx][len]) len++;
    for (int i = 0; i <= len; i++) input_buf[i] = history[idx][i];
    input_len = len; input_cur = len;
    input_redraw();
}

static void history_down(void) {
    if (history_pos <= 0) {
        history_pos = -1; input_len = 0; input_cur = 0; input_buf[0] = 0;
        input_redraw(); return;
    }
    history_pos--;
    int idx = (history_head - 1 - history_pos + HISTORY_SIZE * 2) % HISTORY_SIZE;
    int len = 0; while (history[idx][len]) len++;
    for (int i = 0; i <= len; i++) input_buf[i] = history[idx][i];
    input_len = len; input_cur = len;
    input_redraw();
}

/* -----------------------------------------------------------------------
   Tab completion
   ----------------------------------------------------------------------- */
static const char *tab_cmds[] = {
    "help","clear","cls","echo","mem","about","reboot","shutdown",
    "color","repeat","ls","cat","write","mkdir","cd","pwd",
    "rm","tree","uptime","whoami","uname",
    "save","load","dsave","dls","drm","df",
    "edit","paging","ps","kill","spawn","yield",
    "mount","use","unmount","passwd","calc","zoom", NULL
};

static void tab_complete(void) {
    int start = input_cur;
    while (start > 0 && input_buf[start-1] != ' ') start--;
    int wlen = input_cur - start;
    if (wlen == 0) return;
    char word[64];
    for (int i = 0; i < wlen && i < 63; i++) word[i] = input_buf[start+i];
    word[wlen] = 0;

    int is_first = 1;
    for (int i = 0; i < start; i++)
        if (input_buf[i] != ' ') { is_first = 0; break; }

    const char *match = NULL;
    int match_count = 0;

    if (is_first) {
        for (int i = 0; tab_cmds[i]; i++) {
            int ok = 1;
            for (int j = 0; j < wlen; j++)
                if (tab_cmds[i][j] != word[j]) { ok = 0; break; }
            if (ok) {
                if (tab_cmds[i][wlen] == 0) {
                    /* exact — add space */
                    if (input_len < INPUT_MAX) {
                        for (int j = input_len; j > input_cur; j--) input_buf[j] = input_buf[j-1];
                        input_buf[input_cur] = ' '; input_len++; input_cur++;
                        input_buf[input_len] = 0; input_redraw();
                    }
                    return;
                }
                match = tab_cmds[i]; match_count++;
            }
        }
    } else {
        vfs_node_t *dir = vfs_get_cwd();
        for (int i = 0; i < dir->child_count; i++) {
            const char *name = dir->children[i]->name;
            int ok = 1;
            for (int j = 0; j < wlen; j++)
                if (name[j] != word[j]) { ok = 0; break; }
            if (ok) { match = name; match_count++; }
        }
    }

    if (match_count == 1 && match) {
        /* Replace the partial word (from start to input_cur) with full match */
        int full_len = 0; while (match[full_len]) full_len++;
        int tail_len = input_len - input_cur; /* chars after cursor */

        /* Build new buffer: text before word + full match + tail + optional space */
        char newbuf[INPUT_MAX + 2];
        int  pos = 0;
        /* Copy text before the word */
        for (int i = 0; i < start && pos < INPUT_MAX; i++)
            newbuf[pos++] = input_buf[i];
        /* Insert full match */
        for (int i = 0; i < full_len && pos < INPUT_MAX; i++)
            newbuf[pos++] = match[i];
        /* Add trailing space for commands */
        if (is_first && pos < INPUT_MAX)
            newbuf[pos++] = ' ';
        /* Copy tail (text after original cursor) */
        for (int i = 0; i < tail_len && pos < INPUT_MAX; i++)
            newbuf[pos++] = input_buf[input_cur + i];
        newbuf[pos] = 0;

        /* Write back */
        for (int i = 0; i <= pos; i++) input_buf[i] = newbuf[i];
        input_len = pos;
        input_cur = start + full_len + (is_first ? 1 : 0);
        input_buf[input_len] = 0;
        input_redraw();
    }
}

/* -----------------------------------------------------------------------
   State
   ----------------------------------------------------------------------- */
static int shift_pressed = 0;
static int caps_lock     = 0;
static int ctrl_pressed  = 0;  /* tracks Ctrl key state */
static int extended_key  = 0;
static volatile char last_char = 0;

#define EXT_PAGE_UP   0x49
#define EXT_PAGE_DOWN 0x51
#define EXT_UP        0x48
#define EXT_DOWN      0x50
#define EXT_LEFT      0x4B
#define EXT_RIGHT     0x4D
#define EXT_HOME      0x47
#define EXT_END       0x4F
#define EXT_DELETE    0x53

/* -----------------------------------------------------------------------
   IRQ1 handler
   ----------------------------------------------------------------------- */
static void keyboard_irq_handler(registers_t *regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    /* Route to editor when active */
    if (editor_active()) {
        if (scancode == 0xE0) { extended_key = 1; return; }
        if (scancode & 0x80) {
            uint8_t rel = scancode & 0x7F;
            if (!extended_key) {
                if (rel==0x2A||rel==0x36) shift_pressed=0;
                if (rel==0x1D) ctrl_pressed=0;
            }
            extended_key=0; return;
        }
        if (scancode==0x2A||scancode==0x36){shift_pressed=1;return;}
        if (scancode==0x1D){ctrl_pressed=1;return;}
        /* Pass extended keys with high bit set so editor can distinguish */
        uint8_t ed_sc = extended_key ? (scancode | 0x80) : scancode;
        extended_key = 0;
        editor_key(ed_sc, shift_pressed, ctrl_pressed);
        return;
    }

    /* E0 prefix = extended key coming next */
    if (scancode == 0xE0) { extended_key = 1; return; }

    /* Key release */
    if (scancode & 0x80) {
        if (!extended_key) {
            uint8_t rel = scancode & 0x7F;
            if (rel == 0x2A || rel == 0x36) shift_pressed = 0;
            if (rel == 0x1D) ctrl_pressed = 0;
        }
        extended_key = 0;
        return;
    }

    /* Extended keys — Page Up/Down, arrows */
    if (extended_key) {
        extended_key = 0;
        switch (scancode) {
            case EXT_PAGE_UP:   terminal_scroll_up();   return;
            case EXT_PAGE_DOWN: terminal_scroll_down(); return;
            case EXT_UP:        history_up();           return;
            case EXT_DOWN:      history_down();         return;
            case EXT_LEFT:      input_move_left();      return;
            case EXT_RIGHT:     input_move_right();     return;
            case EXT_HOME:      input_move_home();      return;
            case EXT_END:       input_move_end();       return;
            case EXT_DELETE:
                /* Delete char AT cursor — shift rest left */
                if (input_cur < input_len) {
                    for (int i = input_cur; i < input_len - 1; i++)
                        input_buf[i] = input_buf[i + 1];
                    input_len--;
                    input_buf[input_len] = 0;
                    input_redraw();
                }
                return;
            default: return;
        }
    }

    /* Ctrl — scancodes 0x1D (left) */
    if (scancode == 0x1D) { ctrl_pressed = 1; return; }

    /* Tab — scancode 0x0F */
    if (scancode == 0x0F) { tab_complete(); return; }

    /* Caps Lock — scancode 0x3A, toggles on press */
    if (scancode == 0x3A) { caps_lock = !caps_lock; return; }

    /* Shift */
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }

    /* Convert to ASCII — Caps Lock flips case for letters only */
    char c = scancode_ascii[scancode];
    if (c == 0) return;
    /* Determine if we should uppercase:
       Caps Lock ON  + no shift → uppercase letters
       Caps Lock ON  + shift    → lowercase letters (shift cancels caps)
       Caps Lock OFF + shift    → uppercase letters (normal shift)
       Caps Lock OFF + no shift → lowercase (normal) */
    int want_upper = caps_lock ^ shift_pressed;
    if (want_upper && c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';                   /* letter → uppercase */
    else if (shift_pressed && !(c >= 'a' && c <= 'z'))
        c = scancode_shift[scancode];         /* symbol/number → shifted version */
    if (c == 0) return;

    last_char = c;

    /* Handle Ctrl+key combos — must be checked AFTER ASCII conversion */
    if (ctrl_pressed) {
        /* Ctrl+V — paste clipboard into input bar */
        if (scancode == 0x2F) {
            const char *clip = clipboard_get();
            int clen = clipboard_len();
            for (int i = 0; i < clen && input_len < INPUT_MAX; i++) {
                char pc = clip[i];
                if (pc == '\n' || pc == '\r') continue;
                for (int j = input_len; j > input_cur; j--)
                    input_buf[j] = input_buf[j-1];
                input_buf[input_cur++] = pc;
                input_len++;
            }
            input_buf[input_len] = 0;
            input_redraw();
        }
        /* Ctrl+C — copy current input line to clipboard */
        if (scancode == 0x2E) {
            clipboard_copy(input_buf, input_len);
        }
        /* Ctrl+X — cut current input line */
        if (scancode == 0x2D) {
            clipboard_copy(input_buf, input_len);
            input_len = 0; input_cur = 0; input_buf[0] = 0;
            input_redraw();
        }
        /* Ctrl+A — go to start of line */
        if (scancode == 0x1E) input_move_home();
        /* Ctrl+E — go to end of line */
        if (scancode == 0x12) input_move_end();
        /* Ctrl++ (0x0D = = key, same key as +) — zoom in */
        if (scancode == 0x0D) { terminal_zoom_in();  return; }
        /* Ctrl+- (0x0C = - key) — zoom out */
        if (scancode == 0x0C) { terminal_zoom_out(); return; }
        /* Ctrl+0 (0x0B = 0 key) — zoom reset */
        if (scancode == 0x0B) { terminal_zoom_reset(); return; }
        return; /* don't send Ctrl combos to shell */
    }

    if (c == '\n') {
        /* Save buffer BEFORE input_putchar clears it */
        char snap[INPUT_MAX + 1];
        int  snap_len = input_len;
        for (int i = 0; i <= snap_len; i++) snap[i] = input_buf[i];

        /* Push to history using saved snapshot */
        if (snap_len > 0) {
            int last = (history_head - 1 + HISTORY_SIZE) % HISTORY_SIZE;
            int same = 0;
            if (history_count > 0) {
                same = 1;
                for (int i = 0; i <= snap_len; i++)
                    if (history[last][i] != snap[i]) { same = 0; break; }
            }
            if (!same) {
                for (int i = 0; i <= snap_len; i++) history[history_head][i] = snap[i];
                history_head = (history_head + 1) % HISTORY_SIZE;
                if (history_count < HISTORY_SIZE) history_count++;
            }
            history_pos = -1;
        }

        /* Clear input bar */
        input_putchar('\n');

        /* Feed saved buffer to shell */
        for (int i = 0; i < snap_len; i++) shell_feed(snap[i]);
        shell_feed('\n');

    } else if (c == '\b') {
        input_putchar('\b');
        /* Don't feed backspace to shell — we send full buffer on Enter */
    } else {
        /* Write to input bar only — shell gets full buffer on Enter */
        input_putchar(c);
    }
}

/* -----------------------------------------------------------------------
   keyboard_init
   ----------------------------------------------------------------------- */
void keyboard_init(void) {
    irq_install_handler(1, keyboard_irq_handler);

    /* Unmask IRQ1 */
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 1));

    /* Draw the fixed UI elements */
    draw_separator();
    draw_input_prompt();

}

char keyboard_getchar(void) {
    char c = last_char;
    last_char = 0;
    return c;
}

/* Called by shell after cd to update the path in the prompt immediately */
void keyboard_redraw_prompt(void) {
    input_redraw();
}
