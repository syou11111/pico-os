.syntax unified
.cpu cortex-m0plus
.thumb

.global isr_pendsv
.type isr_pendsv, %function

.extern current_task
.extern os_schedule_next

/*
 * Cortex-M0+ PendSV Context Switcher
 * Saves r4-r11 onto the current task's PSP, calls os_schedule_next(),
 * and restores r4-r11 from the next task's PSP.
 */
isr_pendsv:
    cpsid   i                   /* Disable interrupts during switch */

    ldr     r1, =current_task
    ldr     r2, [r1]            /* r2 = current_task */
    cmp     r2, #0
    beq     restore_next        /* First launch: skip saving MSP context */

    /* Save r4-r11 onto current task's PSP */
    mrs     r0, psp
    subs    r0, r0, #32
    stmia   r0!, {r4-r7}        /* Save r4-r7 at [sp+0..16) */
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    stmia   r0!, {r4-r7}        /* Save r8-r11 at [sp+16..32) */
    subs    r0, r0, #32         /* Rewind r0 to bottom of saved frame */
    str     r0, [r2]            /* current_task->sp = r0 */

restore_next:
    bl      os_schedule_next    /* Updates current_task to next runnable TCB */

    ldr     r1, =current_task
    ldr     r2, [r1]
    ldr     r0, [r2]            /* r0 = current_task->sp */

    /* Restore r8-r11 from [sp+16..32) */
    adds    r0, r0, #16
    ldmia   r0!, {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7

    /* Restore r4-r7 from [sp+0..16) */
    subs    r0, r0, #32
    ldmia   r0!, {r4-r7}
    adds    r0, r0, #16         /* Point PSP to hardware-saved frame */

    msr     psp, r0

    ldr     r0, =0xFFFFFFFD     /* EXC_RETURN: Thread mode, use PSP */
    cpsie   i                   /* Re-enable interrupts */
    bx      r0
