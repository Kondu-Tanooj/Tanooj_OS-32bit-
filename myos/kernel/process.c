/*
 * process.c — cooperative + preemptive multitasking for TanoojOS
 *
 * Each task gets a 4 KiB private stack. The scheduler is triggered
 * by IRQ0 (timer, 18 Hz) and uses round-robin selection.
 *
 * Context switch:
 *   We save the current task's ESP, then load the next task's ESP.
 *   Because all registers were pushed onto the stack by the IRQ handler,
 *   loading the new ESP automatically gives us the new task's registers.
 *
 * Kernel tasks only (ring 0). User-mode processes come in Phase 9.
 */

#include "process.h"
#include "terminal.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   Helpers
   ----------------------------------------------------------------------- */
static void str_copy(char *d, const char *s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]=0;
}
static void mem_zero(void *p, int n) {
    uint8_t *b=(uint8_t*)p; for(int i=0;i<n;i++) b[i]=0;
}

/* -----------------------------------------------------------------------
   Task pool (static — no heap needed)
   ----------------------------------------------------------------------- */
static task_t  task_pool[MAX_TASKS];
static task_t *current   = NULL;   /* currently running task       */
static task_t *run_queue = NULL;   /* head of circular linked list */
static uint32_t next_pid = 1;
static int      task_count = 0;
static int      scheduler_ready = 0;

task_t *process_current(void) { return current; }
int     process_count(void)   { return task_count; }

/* -----------------------------------------------------------------------
   Allocate a slot from the pool
   ----------------------------------------------------------------------- */
static task_t *pool_alloc(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_DEAD) {
            mem_zero(&task_pool[i], sizeof(task_t));
            task_pool[i].state = TASK_READY;
            return &task_pool[i];
        }
    }
    return NULL;
}

/* -----------------------------------------------------------------------
   Insert task into circular run queue
   ----------------------------------------------------------------------- */
static void queue_insert(task_t *t) {
    if (!run_queue) {
        run_queue = t;
        t->next   = t;
    } else {
        /* Insert after current head */
        t->next         = run_queue->next;
        run_queue->next = t;
    }
    task_count++;
}

/* -----------------------------------------------------------------------
   Remove task from circular run queue
   ----------------------------------------------------------------------- */
static void queue_remove(task_t *t) {
    if (!run_queue) return;
    if (run_queue == t && t->next == t) {
        run_queue = NULL;
        task_count = 0;
        return;
    }
    task_t *prev = run_queue;
    while (prev->next != t) {
        prev = prev->next;
        if (prev == run_queue) return; /* not found */
    }
    prev->next = t->next;
    if (run_queue == t) run_queue = t->next;
    task_count--;
}

/* -----------------------------------------------------------------------
   Context switch — pure C version using inline asm
   Saves current ESP, loads next ESP, returns to new task.
   ----------------------------------------------------------------------- */
static void switch_to(task_t *next) {
    if (current == next) return;
    task_t *prev = current;
    current = next;

    /* Save current ESP into prev->esp, load next->esp into ESP.
       This is the heart of context switching. */
    __asm__ volatile (
        "mov %%esp, %0\n"   /* save current stack pointer */
        "mov %1, %%esp\n"   /* load next task's stack pointer */
        : "=m"(prev->esp)
        : "m"(next->esp)
        : "memory"
    );
}

/* -----------------------------------------------------------------------
   schedule() — called from IRQ0 timer handler
   ----------------------------------------------------------------------- */
static volatile uint32_t sched_ticks = 0;
#define TIME_SLICE 3   /* timer ticks per task (3/18 ≈ 166ms) */

void schedule(void) {
    if (!scheduler_ready || !run_queue) return;

    sched_ticks++;

    /* Decrement sleep counters */
    task_t *t = run_queue;
    do {
        if (t->state == TASK_SLEEPING) {
            if (t->sleep_ticks > 0) t->sleep_ticks--;
            if (t->sleep_ticks == 0) t->state = TASK_READY;
        }
        t = t->next;
    } while (t != run_queue);

    /* Only switch every TIME_SLICE ticks */
    if (sched_ticks % TIME_SLICE != 0) return;

    /* Find next READY task (round-robin) */
    task_t *start = current ? current->next : run_queue;
    task_t *next  = start;
    do {
        if (next->state == TASK_READY || next->state == TASK_RUNNING)
            break;
        next = next->next;
    } while (next != start);

    if (next == current) return; /* no other ready task */
    if (next->state != TASK_READY && next->state != TASK_RUNNING) return;

    if (current && current->state == TASK_RUNNING)
        current->state = TASK_READY;
    next->state = TASK_RUNNING;

    switch_to(next);
}

/* -----------------------------------------------------------------------
   process_yield — voluntarily give up CPU
   ----------------------------------------------------------------------- */
void process_yield(void) {
    sched_ticks = TIME_SLICE - 1; /* force switch on next timer */
    __asm__ volatile ("int $0x20"); /* trigger timer IRQ manually */
}

/* -----------------------------------------------------------------------
   process_sleep — sleep for N timer ticks
   ----------------------------------------------------------------------- */
void process_sleep(uint32_t ticks) {
    if (!current) return;
    current->state       = TASK_SLEEPING;
    current->sleep_ticks = ticks;
    process_yield();
}

/* -----------------------------------------------------------------------
   Set up initial stack for a new task
   When the task is first switched to, it will "return" to task_fn.
   ----------------------------------------------------------------------- */
static void setup_stack(task_t *t, task_fn_t fn) {
    /* Point ESP at top of stack */
    uint32_t *sp = (uint32_t *)(t->stack + STACK_SIZE);

    /* Push a fake return address (task should never return) */
    sp--; *sp = 0xDEADBEEF;

    /* Push the task function address — when switched to,
       the CPU will "return" here */
    sp--; *sp = (uint32_t)fn;

    /* Push dummy saved registers (as if we had done pushad) */
    sp--; *sp = 0; /* eax */
    sp--; *sp = 0; /* ecx */
    sp--; *sp = 0; /* edx */
    sp--; *sp = 0; /* ebx */
    sp--; *sp = 0; /* esp (ignored) */
    sp--; *sp = 0; /* ebp */
    sp--; *sp = 0; /* esi */
    sp--; *sp = 0; /* edi */

    t->esp = (uint32_t)sp;
}

/* -----------------------------------------------------------------------
   process_create — create a new kernel task
   ----------------------------------------------------------------------- */
task_t *process_create(const char *name, task_fn_t fn) {
    task_t *t = pool_alloc();
    if (!t) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  [PROC] No free task slots!");
        return NULL;
    }

    t->pid         = next_pid++;
    t->state       = TASK_READY;
    t->sleep_ticks = 0;
    t->next        = NULL;
    str_copy(t->name, name, 32);

    setup_stack(t, fn);
    queue_insert(t);

    return t;
}

/* -----------------------------------------------------------------------
   process_kill — mark a task as dead
   ----------------------------------------------------------------------- */
void process_kill(uint32_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].pid == pid && task_pool[i].state != TASK_DEAD) {
            queue_remove(&task_pool[i]);
            task_pool[i].state = TASK_DEAD;
            return;
        }
    }
}

/* -----------------------------------------------------------------------
   process_list — print task table to terminal
   ----------------------------------------------------------------------- */
void process_list(void) {
    terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    terminal_writeline("  PID  State     Name");
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("  ---  --------  ----");

    static const char *state_names[] = {
        "running ", "ready   ", "sleeping", "dead    "
    };

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &task_pool[i];
        if (t->state == TASK_DEAD) continue;
        terminal_set_color(
            t->state == TASK_RUNNING ? COLOR_LIGHT_GREEN : COLOR_WHITE,
            COLOR_BLACK
        );
        terminal_write("  ");
        terminal_write_dec(t->pid);
        terminal_write("    ");
        terminal_write(state_names[t->state < 4 ? t->state : 3]);
        terminal_write("  ");
        terminal_writeline(t->name);
    }
}

/* -----------------------------------------------------------------------
   process_init — set up the kernel main task and start scheduling
   ----------------------------------------------------------------------- */
void process_init(void) {
    /* Mark all slots as dead */
    for (int i = 0; i < MAX_TASKS; i++)
        task_pool[i].state = TASK_DEAD;

    task_count = 0;
    next_pid   = 1;
    sched_ticks = 0;

    /* Register the currently running code as "kernel" task */
    task_t *kernel_task = &task_pool[0];
    mem_zero(kernel_task, sizeof(task_t));
    kernel_task->pid   = next_pid++;
    kernel_task->state = TASK_RUNNING;
    str_copy(kernel_task->name, "kernel", 32);
    kernel_task->next  = kernel_task; /* circular, just itself for now */
    run_queue  = kernel_task;
    current    = kernel_task;
    task_count = 1;

    scheduler_ready = 1;

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  [PROC] Multitasking ready. Use 'ps' to list tasks.");
}
