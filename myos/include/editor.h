#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>

/* Launch the editor. If filename is NULL, opens an empty buffer. */
void editor_open(const char *filename);

/* Returns 1 if editor is currently active */
int editor_active(void);

/* Feed a keypress to the editor (called from keyboard handler) */
void editor_key(uint8_t scancode, int shift, int ctrl);

#endif
