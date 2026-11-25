#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Mouse button flags */
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* Mouse state — read by other modules */
typedef struct {
    int x, y;           /* current position in VGA cells (col, row) */
    int buttons;        /* bitmask of pressed buttons                */
    int dx, dy;         /* last movement delta                       */
} mouse_state_t;

void  mouse_init(void);
mouse_state_t *mouse_get_state(void);

/* Clipboard */
void        clipboard_copy(const char *text, int len);
const char *clipboard_get(void);
int         clipboard_len(void);

#endif
