#ifndef SHELL_H
#define SHELL_H

/* Initialise and start the shell — call from kernel_main after all init */
void shell_init(void);

/* Feed one character to the shell — call from keyboard handler on Enter */
void shell_feed(char c);

#endif
