#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>

#define VGA_ADDRESS  0xB8000
#define VGA_WIDTH    80
#define VGA_HEIGHT   23   /* rows 23+24 reserved for input bar + status bar */

typedef enum {
    COLOR_BLACK         = 0,
    COLOR_BLUE          = 1,
    COLOR_GREEN         = 2,
    COLOR_CYAN          = 3,
    COLOR_RED           = 4,
    COLOR_MAGENTA       = 5,
    COLOR_BROWN         = 6,
    COLOR_LIGHT_GREY    = 7,
    COLOR_DARK_GREY     = 8,
    COLOR_LIGHT_BLUE    = 9,
    COLOR_LIGHT_GREEN   = 10,
    COLOR_LIGHT_CYAN    = 11,
    COLOR_LIGHT_RED     = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN   = 14,
    COLOR_WHITE         = 15,
} vga_color_t;

void terminal_init(void);
void terminal_set_color(vga_color_t fg, vga_color_t bg);
void terminal_putchar(char c);
void terminal_write(const char *str);
void terminal_writeline(const char *str);
void terminal_write_hex(uint32_t value);
void terminal_write_dec(uint32_t value);

/* Scroll-back buffer controls */
void terminal_scroll_up(void);    /* Page Up   — scroll back in history */
void terminal_scroll_down(void);  /* Page Down — scroll forward          */

void terminal_zoom_in(void);
void terminal_zoom_out(void);
void terminal_zoom_reset(void);

#endif
