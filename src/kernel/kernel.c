#include "kernel.h"
#include "hardware/clocks.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include <string.h>

#define SCB_ICSR_PENDSVSET_Msk  (1UL << 28)

tcb_t *volatile current_task = NULL;
static tcb_t tasks[OS_MAX_TASKS];
static volatile uint32_t os_ticks = 0;
static uint32_t task_count = 0;

static void task_exit_trap(void) {
    while (1) {
        os_sleep_ms(1000);
    }
}

static void idle_task(void *arg) {
    (void)arg;
    while (1) {
        __asm volatile("wfi");
    }
}

uint32_t os_enter_critical(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask\ncpsid i" : "=r"(primask) :: "memory");
    return primask;
}

void os_exit_critical(uint32_t primask) {
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
}

void os_init(void) {
    memset(tasks, 0, sizeof(tasks));
    current_task = NULL;
    os_ticks = 0;
    task_count = 0;

    /* Task 0 is always the idle task */
    os_create_task("idle", idle_task, NULL);
}

int os_create_task(const char *name, task_fn_t entry, void *arg) {
    if (task_count >= OS_MAX_TASKS) {
        return -1;
    }

    uint32_t primask = os_enter_critical();

    tcb_t *t = &tasks[task_count];
    t->id = task_count;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->state = TASK_READY;
    t->wake_tick = 0;
    t->cpu_ticks = 0;

    /* Build initial stack frame matching context_switch.S */
    uint32_t *sp = &t->stack[OS_STACK_WORDS];

    /* Hardware-saved frame (pushed/popped by Cortex-M0+ exception) */
    *(--sp) = 0x01000000UL;               /* xPSR (Thumb bit set) */
    *(--sp) = (uint32_t)entry;            /* PC */
    *(--sp) = (uint32_t)task_exit_trap;   /* LR */
    *(--sp) = 0x12121212UL;               /* R12 */
    *(--sp) = 0x03030303UL;               /* R3 */
    *(--sp) = 0x02020202UL;               /* R2 */
    *(--sp) = 0x01010101UL;               /* R1 */
    *(--sp) = (uint32_t)arg;              /* R0 */

    /* Software-saved frame: R8-R11 at [sp+16..32), R4-R7 at [sp+0..16) */
    *(--sp) = 0x11111111UL;               /* R11 */
    *(--sp) = 0x10101010UL;               /* R10 */
    *(--sp) = 0x09090909UL;               /* R9 */
    *(--sp) = 0x08080808UL;               /* R8 */
    *(--sp) = 0x07070707UL;               /* R7 */
    *(--sp) = 0x06060606UL;               /* R6 */
    *(--sp) = 0x05050505UL;               /* R5 */
    *(--sp) = 0x04040404UL;               /* R4 */

    t->sp = sp;
    task_count++;

    os_exit_critical(primask);
    return (int)t->id;
}

void os_schedule_next(void) {
    if (current_task && current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }

    uint32_t start = current_task ? current_task->id : 0;

    /* Round-robin across non-idle tasks (1 .. task_count-1) */
    if (task_count > 1) {
        for (uint32_t offset = 1; offset <= task_count; offset++) {
            uint32_t idx = (start + offset) % task_count;
            if (idx == 0) {
                continue; /* Prefer non-idle tasks first */
            }
            if (tasks[idx].state == TASK_READY) {
                current_task = &tasks[idx];
                current_task->state = TASK_RUNNING;
                return;
            }
        }
    }

    /* Fall back to idle task */
    current_task = &tasks[0];
    current_task->state = TASK_RUNNING;
}

void os_yield(void) {
    scb_hw->icsr = SCB_ICSR_PENDSVSET_Msk;
    __asm volatile("dsb\nisb");
}

void os_sleep_ms(uint32_t ms) {
    if (ms == 0) {
        os_yield();
        return;
    }
    uint32_t primask = os_enter_critical();
    current_task->wake_tick = os_ticks + ms;
    current_task->state = TASK_SLEEPING;
    os_exit_critical(primask);

    os_yield();
}

uint32_t os_get_ticks(void) {
    return os_ticks;
}

uint32_t os_get_task_count(void) {
    return task_count;
}

const tcb_t *os_get_tasks(void) {
    return tasks;
}

void isr_systick(void) {
    os_ticks++;

    if (current_task) {
        current_task->cpu_ticks++;
    }

    for (uint32_t i = 1; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING && (int32_t)(os_ticks - tasks[i].wake_tick) >= 0) {
            tasks[i].state = TASK_READY;
        }
    }

    /* Request context switch */
    scb_hw->icsr = SCB_ICSR_PENDSVSET_Msk;
}

void os_start(void) {
    /* Set PendSV and SysTick to lowest priority (SHPR3 at 0xE000ED20) */
    (*(volatile uint32_t *)0xE000ED20UL) |= (0xFFUL << 16) | (0xFFUL << 24);

    /* Configure SysTick for 1ms tick */
    uint32_t reload = (clock_get_hz(clk_sys) / OS_TICK_HZ) - 1;
    systick_hw->rvr = reload;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x07; /* ENABLE | TICKINT | CLKSOURCE (clk_sys) */

    /* Trigger initial PendSV to switch from MSP to Task 0/1 PSP */
    scb_hw->icsr = SCB_ICSR_PENDSVSET_Msk;
    __asm volatile("cpsie i\ndsb\nisb");

    /* Should never reach here */
    while (1) {
        __asm volatile("wfi");
    }
}
