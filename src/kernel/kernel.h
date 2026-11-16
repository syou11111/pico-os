#ifndef PICO_OS_KERNEL_H
#define PICO_OS_KERNEL_H

#include <stdint.h>
#include <stdbool.h>

#define OS_MAX_TASKS       8
#define OS_STACK_WORDS     2048 /* 8 KB per task */
#define OS_TICK_HZ         1000 /* 1 ms system tick */

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING
} task_state_t;

typedef struct {
    uint32_t    *sp;           /* Must be at offset 0 for context_switch.S */
    uint32_t     id;
    char         name[16];
    task_state_t state;
    uint32_t     wake_tick;
    uint32_t     cpu_ticks;
    uint32_t     stack[OS_STACK_WORDS];
} tcb_t;

typedef void (*task_fn_t)(void *arg);

/* Kernel API */
void     os_init(void);
int      os_create_task(const char *name, task_fn_t entry, void *arg);
void     os_start(void);
void     os_sleep_ms(uint32_t ms);
void     os_yield(void);
uint32_t os_get_ticks(void);
uint32_t os_get_task_count(void);
const tcb_t *os_get_tasks(void);

/* Simple spinlock/critical section helpers */
uint32_t os_enter_critical(void);
void     os_exit_critical(uint32_t primask);

#endif /* PICO_OS_KERNEL_H */
