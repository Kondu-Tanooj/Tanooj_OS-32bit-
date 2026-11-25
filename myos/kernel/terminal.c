/*
 * terminal.c — VGA terminal using a doubly-linked list of row nodes
 *
 * Architecture:
 *   - A static pool of ROW_POOL_SIZE row_node_t structs (no heap needed)
 *   - All rows (history + visible) live in a doubly-linked list
 *   - view_start  → first row currently shown on screen
 *   - current_row → row the cursor is writing to (always in visible area)
 *   - Scrolling   → just move view_start forward/backward along the list
 *   - Auto-scroll → when new row added past bottom, view_start = view_start->next
 *   - Recycling   → when list exceeds ROW_POOL_SIZE, head node is detached and reused
 *
 * No copying, no corruption — scrolling is O(1) pointer movement.
 */

#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   Constants
   ----------------------------------------------------------------------- */
#define ROW_POOL_SIZE  300    /* max rows in list (history + visible)  */
#define SCROLL_STEP    5      /* rows moved per Page Up/Down           */

/* -----------------------------------------------------------------------
   Row node
   ----------------------------------------------------------------------- */
typedef struct row_node {
    uint16_t         cells[VGA_WIDTH];  /* one full screen row              */
    struct row_node *prev;              /* older row (toward head)          */
    struct row_node *next;              /* newer row (toward tail)          */
    int              in_use;            /* 1 = part of live list            */
} row_node_t;

/* -----------------------------------------------------------------------
   Node pool — static allocation, no heap dependency
   ----------------------------------------------------------------------- */
static row_node_t pool[ROW_POOL_SIZE];
static int        pool_idx = 0;  /* simple round-robin recycler */

static row_node_t *pool_alloc(void) {
    /* Find a free node; if the pool is full, steal the list head */
    row_node_t *n = &pool[pool_idx];
    pool_idx = (pool_idx + 1) % ROW_POOL_SIZE;
    return n;
}

/* -----------------------------------------------------------------------
   List state
   ----------------------------------------------------------------------- */
static row_node_t *list_head  = NULL;  /* oldest row in history            */
static row_node_t *list_tail  = NULL;  /* newest row (current write target)*/
static int         list_size  = 0;

static row_node_t *view_start = NULL;  /* first row shown on screen        */
static row_node_t *cur_node   = NULL;  /* row cursor is writing to         */
static int         cur_col    = 0;     /* column cursor is at              */
static int         scroll_mode = 0;    /* 1 = user is scrolling            */

/* -----------------------------------------------------------------------
   VGA access
   ----------------------------------------------------------------------- */
static volatile uint16_t * const VGA = (volatile uint16_t *)0xB8000;

static inline uint8_t  make_color(vga_color_t fg, vga_color_t bg) {
    return (uint8_t)(fg | (bg << 4));
}
static inline uint16_t make_cell(char c, uint8_t color) {
    return (uint16_t)(unsigned char)c | ((uint16_t)color << 8);
}

static uint8_t cur_color;

/* -----------------------------------------------------------------------
   Render: paint VGA_HEIGHT rows starting from view_start
   ----------------------------------------------------------------------- */
static void render(void) {
    row_node_t *n = view_start;
    for (int r = 0; r < VGA_HEIGHT; r++) {
        if (n) {
            for (int c = 0; c < VGA_WIDTH; c++)
                VGA[r * VGA_WIDTH + c] = n->cells[c];
            n = n->next;
        } else {
            /* Past the end of the list — blank row */
            uint16_t blank = make_cell(' ', cur_color);
            for (int c = 0; c < VGA_WIDTH; c++)
                VGA[r * VGA_WIDTH + c] = blank;
        }
    }
}

/* -----------------------------------------------------------------------
   Scroll indicator on last row
   ----------------------------------------------------------------------- */
static void render_scroll_indicator(void) {
    uint8_t color = make_color(COLOR_WHITE, COLOR_CYAN);
    for (int c = 0; c < VGA_WIDTH; c++)
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = make_cell(' ', color);
    const char *msg = "  [SCROLL]  PgUp=back  PgDn=forward  any key=exit";
    for (int i = 0; msg[i] && i < VGA_WIDTH; i++)
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = make_cell(msg[i], color);
}

/* -----------------------------------------------------------------------
   How many nodes from view_start to cur_node (0 = cur_node is view_start)
   ----------------------------------------------------------------------- */
static int cursor_screen_row(void) {
    int row = 0;
    row_node_t *n = view_start;
    while (n && n != cur_node && row < VGA_HEIGHT) {
        n = n->next;
        row++;
    }
    return row;
}

/* -----------------------------------------------------------------------
   Append a new blank node at the tail of the list
   ----------------------------------------------------------------------- */
static row_node_t *append_row(void) {
    row_node_t *n = pool_alloc();

    /* If this node was previously in the list, detach it first */
    if (n->in_use) {
        if (n->prev) n->prev->next = n->next;
        if (n->next) n->next->prev = n->prev;
        if (n == list_head) list_head = n->next;
        if (n == view_start) view_start = n->next;
        list_size--;
    }

    /* Blank the node */
    uint16_t blank = make_cell(' ', cur_color);
    for (int c = 0; c < VGA_WIDTH; c++) n->cells[c] = blank;
    n->prev   = list_tail;
    n->next   = NULL;
    n->in_use = 1;

    if (list_tail) list_tail->next = n;
    list_tail = n;
    if (!list_head) list_head = n;
    list_size++;

    return n;
}

/* -----------------------------------------------------------------------
   Auto-scroll: advance view_start when content goes past screen bottom
   ----------------------------------------------------------------------- */
static void auto_scroll(void) {
    /* Count how many rows from view_start to cur_node */
    int row = cursor_screen_row();
    while (row >= VGA_HEIGHT) {
        if (view_start && view_start->next)
            view_start = view_start->next;
        row--;
    }
}

/* -----------------------------------------------------------------------
   terminal_init
   ----------------------------------------------------------------------- */
void terminal_init(void) {
    /* Reset pool */
    for (int i = 0; i < ROW_POOL_SIZE; i++) {
        pool[i].prev   = NULL;
        pool[i].next   = NULL;
        pool[i].in_use = 0;
    }
    pool_idx  = 0;
    list_head = NULL;
    list_tail = NULL;
    list_size = 0;
    scroll_mode = 0;

    cur_color = make_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    cur_col   = 0;

    /* Seed the list with VGA_HEIGHT blank rows */
    for (int r = 0; r < VGA_HEIGHT; r++) {
        row_node_t *n = append_row();
        if (r == 0) view_start = n;
    }
    cur_node = list_tail;

    render();
}

void terminal_set_color(vga_color_t fg, vga_color_t bg) {
    cur_color = make_color(fg, bg);
}

/* -----------------------------------------------------------------------
   Newline: move to the next row node (appending if needed)
   ----------------------------------------------------------------------- */
static void newline(void) {
    cur_col = 0;
    if (cur_node->next) {
        cur_node = cur_node->next;
    } else {
        cur_node = append_row();
    }
    auto_scroll();
}

/* -----------------------------------------------------------------------
   terminal_putchar
   ----------------------------------------------------------------------- */
void terminal_putchar(char c) {
    /* Any output exits scroll mode */
    if (scroll_mode) {
        scroll_mode = 0;
        render();
        return; /* re-entry: let the character be processed next call */
    }

    if (c == '\n') {
        newline();
        render();
        return;
    }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') {
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= VGA_WIDTH) newline();
        render();
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            cur_node->cells[cur_col] = make_cell(' ', cur_color);
            int row = cursor_screen_row();
            if (row < VGA_HEIGHT)
                VGA[row * VGA_WIDTH + cur_col] = cur_node->cells[cur_col];
        }
        return;
    }

    cur_node->cells[cur_col] = make_cell(c, cur_color);
    int row = cursor_screen_row();
    if (row < VGA_HEIGHT)
        VGA[row * VGA_WIDTH + cur_col] = cur_node->cells[cur_col];

    if (++cur_col >= VGA_WIDTH) newline();
}

void terminal_write(const char *s)     { for (int i = 0; s[i]; i++) terminal_putchar(s[i]); }
void terminal_writeline(const char *s) { terminal_write(s); terminal_putchar('\n'); }

void terminal_write_hex(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[11]; buf[0]='0'; buf[1]='x'; buf[10]=0;
    for (int i = 9; i >= 2; i--) { buf[i] = h[v&0xF]; v >>= 4; }
    terminal_write(buf);
}

void terminal_write_dec(uint32_t v) {
    char buf[12]; int pos = 10; buf[11] = 0;
    if (v == 0) { terminal_putchar('0'); return; }
    while (v > 0) { buf[pos--] = '0' + (char)(v%10); v /= 10; }
    terminal_write(&buf[pos+1]);
}

/* -----------------------------------------------------------------------
   Scroll API — O(1) pointer movement along the list
   ----------------------------------------------------------------------- */
void terminal_scroll_up(void) {
    /* Move view_start backward SCROLL_STEP times */
    int moved = 0;
    for (int i = 0; i < SCROLL_STEP; i++) {
        if (view_start && view_start->prev) {
            view_start = view_start->prev;
            moved++;
        }
    }
    if (moved > 0) {
        scroll_mode = 1;
        render();
        render_scroll_indicator();
    }
}

void terminal_scroll_down(void) {
    if (!scroll_mode) return;
    int moved = 0;
    for (int i = 0; i < SCROLL_STEP; i++) {
        if (view_start && view_start->next) {
            view_start = view_start->next;
            moved++;
        }
    }
    /* Check if we've scrolled back to live position */
    int live_row = cursor_screen_row();
    if (live_row < VGA_HEIGHT) {
        scroll_mode = 0;
        render();
    } else {
        render();
        render_scroll_indicator();
    }
}

/* -----------------------------------------------------------------------
   VGA zoom — changes character height via CRTC registers
   Port 0x3D4 = CRTC index, 0x3D5 = CRTC data
   Character height register 0x09 controls scanlines per character.
   Default = 15 (16 scanlines = 80x25).
   8 scanlines = 80x50 (zoom out), 31 scanlines = 80x12 (zoom in).
   ----------------------------------------------------------------------- */

static inline void outb_vga(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb_vga(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

static int zoom_level = 1; /* 0=zoomed in, 1=normal, 2=zoomed out */

static void vga_set_char_height(uint8_t height) {
    /* Unprotect CRTC registers 0-7 */
    outb_vga(0x3D4, 0x11);
    uint8_t r11 = inb_vga(0x3D5) & ~0x80;
    outb_vga(0x3D5, r11);

    /* Set maximum scan line (character height - 1) */
    outb_vga(0x3D4, 0x09);
    uint8_t r09 = inb_vga(0x3D5) & 0xE0; /* preserve top 3 bits */
    outb_vga(0x3D5, r09 | (height & 0x1F));

    /* Update cursor size to match */
    outb_vga(0x3D4, 0x0A); /* cursor start */
    outb_vga(0x3D5, (height - 2) & 0x1F);
    outb_vga(0x3D4, 0x0B); /* cursor end */
    outb_vga(0x3D5, (height - 1) & 0x1F);
}

/* Show zoom level silently in status bar only */
static void zoom_show_status(void) {
    static const char *labels[] = { "Zoom: large ", "Zoom: normal", "Zoom: small " };
    const char *lbl = labels[zoom_level < 3 ? zoom_level : 1];
    uint8_t color = 0x70;
    for (int i = 0; i < 12; i++)
        VGA[24 * 80 + 34 + i] = (uint16_t)(unsigned char)lbl[i] | ((uint16_t)color << 8);
}

void terminal_zoom_in(void) {
    if (zoom_level <= 0) return;
    zoom_level--;
    switch (zoom_level) {
        case 0: vga_set_char_height(31); break; /* 80x12 — big   */
        case 1: vga_set_char_height(15); break; /* 80x25 — normal*/
    }
    zoom_show_status();
}

void terminal_zoom_out(void) {
    if (zoom_level >= 2) return;
    zoom_level++;
    switch (zoom_level) {
        case 1: vga_set_char_height(15); break; /* 80x25 — normal */
        case 2: vga_set_char_height(7);  break; /* 80x50 — small  */
    }
    zoom_show_status();
}

void terminal_zoom_reset(void) {
    zoom_level = 1;
    vga_set_char_height(15);
    zoom_show_status();
}
