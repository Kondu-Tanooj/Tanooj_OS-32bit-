#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define MAX_TASKS    16
#define STACK_SIZE   4096   /* 4 KiB per task */
#define TASK_RUNNING 0
#define TASK_READY   1
#define TASK_SLEEPING 2
#define TASK_DEAD    3

typedef void (*task_fn_t)(void);

typedef struct task {
    uint32_t pid;
    char     name[32];
    int      state;
    uint32_t esp;                   /* saved stack pointer         */
    uint8_t  stack[STACK_SIZE];     /* private kernel stack        */
    uint32_t sleep_ticks;           /* ticks remaining if sleeping */
    struct task *next;              /* next in circular list       */
} task_t;

void     process_init(void);
task_t  *process_create(const char *name, task_fn_t fn);
void     process_kill(uint32_t pid);
void     process_sleep(uint32_t ticks);
void     process_yield(void);
void     schedule(void);            /* called from timer IRQ       */
void     process_list(void);        /* print all tasks             */
int      process_count(void);
task_t  *process_current(void);

#endif
