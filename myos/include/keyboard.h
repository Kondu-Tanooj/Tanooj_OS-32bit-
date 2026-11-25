#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
char keyboard_getchar(void);
void keyboard_redraw_prompt(void);   /* Returns last key pressed (0 if none yet) */

#endif
