/**
 * @file    ZenOS.cpp
 * @brief   ZenOS RTOS — Kernel Core (globals, init, start, PendSV, idle, tick, yield, delay)
 *
 * This file contains the kernel core that ties all modules together.
 * Modularized code lives in:
 *   ZenOS_Scheduler.cpp — O(1) bitmap scheduler, task create/control/lookup
 *   ZenOS_IPC.cpp       — Events, Mutex, C++ wrapper classes
 *   ZenOS_Safety.cpp    — Error system, stack check, fault handler, watchdog, CRC, RAM test, MPU
 *   ZenOS_Monitor.cpp   — Stack watermark, CPU usage, deadline, error log
 *
 * @author  Rahman Heidari <rahman.h22@gmail.com> — Raymon Research Team
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS_Internal.hpp"

extern "C" uint32_t SystemCoreClock;


static uint32_t ram_vectors[OS_VECTOR_COUNT] OS_ALIGNED(512);

/* ═══════════════ Shared Globals ═══════════════ */
/* All scheduler globals below are shared between task context, the naked
   PendSV handler and the SysTick/EXTI ISRs.  They MUST be volatile: at
   -O2 the compiler otherwise keeps them cached in registers across the
   asm boundaries (os_yield, OS_PendSV_Handler) and the scheduler then
   switches to stale TCBs — the classic "works at -O0, corrupt at -O2". */
TCB*              volatile task_list    = nullptr;
uint16_t          volatile task_count   = 0;
TCB*              volatile current_task = nullptr;

uint32_t          idle_stack[OS_IDLE_STACK_WORDS] OS_ALIGNED(8);
volatile uint32_t tick_count = 0;

#if OS_TOOL_EVENT
ECB* volatile event_list = nullptr;
int16_t os_event_next_id = 0;
#endif

TCB idle_tcb;
extern "C" TCB* const os_idle_tcb_ptr = &idle_tcb;

volatile uint32_t blocked_count   = 0;

volatile uint32_t idle_ticks      = 0;
volatile bool     os_started      = false;

volatile uint32_t os_ready_bitmap = 0;
TCB* volatile os_pq_head[32] = {nullptr};

volatile uint32_t os_syst_rvr_normal = 0;
static uint32_t fault_stack[OS_FAULT_STACK_WORDS] OS_ALIGNED(8);

volatile uint32_t os_safe_depth = 0;

volatile uint32_t error_total = 0;
volatile OSError  error_last  = OSError::NONE;
volatile uint32_t error_expected     = 0;
volatile uint32_t error_expect_depth = 0;
volatile uint32_t wdg_reset_count      = 0;
volatile uint32_t stack_recovery_count = 0;


/* ═══════════════ Critical Section ═══════════════ */
extern "C" uint32_t os_critical_enter(void) {
    uint32_t old;
    __asm volatile("mrs %0, PRIMASK\n cpsid i\n" : "=r"(old) :: "memory");
    return old;
}

extern "C" void os_critical_exit(uint32_t old) {
    __asm volatile("msr PRIMASK, %0" :: "r"(old) : "memory");
}


/* ═══════════════ Delays ═══════════════ */
extern "C" void os_delay_us(uint32_t us) {
    if (us == 0) return;

    if (os_safe_depth > 0) {
#if OS_HAS_CYCLE_COUNTER
        /* Opt5: DWT busy-wait — exit once the cycle counter passes the deadline */
        uint32_t cycles = (uint32_t)((uint64_t)us * (SystemCoreClock / 1000000UL));
        uint32_t deadline = OS_DWT_CYCCNT + cycles;
        while ((int32_t)(OS_DWT_CYCCNT - deadline) < 0)
            __asm volatile("nop");
#else
        uint32_t n = (uint32_t)((uint64_t)us * (SystemCoreClock / 4000000UL));
        if (n == 0) n = 1;
        while (n--) { __asm volatile("nop"); __asm volatile("nop"); }
#endif
        return;
    }

    if (us >= 1000UL) { os_delay_ms(us / 1000UL); return; }

    if (os_syst_rvr_normal == 0) {
        volatile uint32_t n = us;
        while (n--) {
            for (volatile uint32_t i = 0; i < 10; i++)
                __asm volatile("nop");
        }
        return;
    }

    uint32_t start = os_get_us();
    while ((os_get_us() - start) < us) __asm volatile("nop");
}

extern "C" void os_delay_ms(uint32_t ms) {
    /* Never block from an ISR: os_safe_depth only catches the RAII guards,
       os_in_isr() catches every exception context. */
    if (os_safe_depth > 0 || os_in_isr()) { os_report_error(OSError::SAFE_DELAY_MS); return; }
    if (ms == 0) return;

    uint32_t delay_ticks = os_ms_to_ticks(ms);
    uint32_t cs = os_critical_enter();
    os_block_current(TaskState::BLOCKED, nullptr, 0, delay_ticks);
    os_critical_exit(cs);
    os_yield();
}

/* Opt1: os_yield — direct asm, no function call overhead (~20 cycles saved) */
extern "C" void os_yield(void) {
    __asm volatile(
        "mrs   r0, PRIMASK\n"
        "cpsid i\n"
        "ldr   r1, =os_safe_depth\n"
        "ldr   r1, [r1]\n"
        "cmp   r1, #0\n"
        "bne   1f\n"
        "ldr   r1, =current_task\n"
        "ldr   r1, [r1]\n"
        "cmp   r1, #0\n"
        "beq   1f\n"
        "ldr   r2, =tick_count\n"
        "ldr   r2, [r2]\n"
        "str   r2, [r1, #48]\n"
        "1:\n"
        "msr   PRIMASK, r0\n"
        "ldr   r0, =0xE000ED04\n"
        "ldr   r1, [r0]\n"
        "orr   r1, r1, #0x10000000\n"
        "str   r1, [r0]\n"
        : /* no outputs */
        : /* no inputs */
        : "r0", "r1", "r2", "cc", "memory"
        /* The asm destroys r0/r1/r2 and the condition flags, and writes
           current_task->last_yield_tick + SCB_ICSR.  Without this clobber
           list GCC at -O2 keeps live values in these registers across the
           asm and the scheduler state corrupts. */
    );
}


/* ═══════════════ Tick Handler ═══════════════ */
extern "C" void os_tick(void) {

    /* ═══════ C2 fix: one critical section for the whole tick path ═══════
       os_tickless_process() and os_stack_check_all() mutate global
       scheduler state (priority queues, bitmap, blocked_count). They must
       run with interrupts disabled, otherwise a higher-priority ISR
       (e.g. EXTI calling os_event_signal_from_isr) can corrupt that state
       concurrently. */
    uint32_t cs = os_critical_enter();

    tick_count++;

    if (current_task == &idle_tcb) idle_ticks++;
    else if (current_task && current_task->state == TaskState::RUNNING) {
        current_task->cpu_ticks++;
#if OS_MONITOR_ENABLED
        /* Stack watermark: track lowest SP seen */
        if ((uint32_t)current_task->stack_top < (uint32_t)current_task->peak_sp)
            current_task->peak_sp = current_task->stack_top;
#endif
    }

#if OS_SAFETY_SOFT_WATCHDOG
    /* Check every 1ms (OS_TICKS_PER_MS ticks) to reduce overhead */
    if ((tick_count % OS_TICKS_PER_MS) == 0 &&
        current_task && current_task != &idle_tcb &&
        current_task->state == TaskState::RUNNING) {
        uint32_t elapsed = tick_count - current_task->last_yield_tick;
        uint32_t max_ticks = OS_SAFETY_SOFT_WDG_TIMEOUT_MS * OS_TICKS_PER_MS;
        if (elapsed > max_ticks) {
            os_report_error(OSError::TASK_STUCK);
            wdg_reset_count++;
            /* O(1) scheduler: remove from pq before reset */
            os_pq_remove(current_task);
            if (current_task->wdg_retries < OS_SAFETY_TASK_MAX_RECOVERY) {
                current_task->wdg_retries++;
                os_reset_task_internal(current_task);
            } else {
                current_task->state = TaskState::INACTIVE;
            }
            current_task = nullptr;
        }
    }
#endif

#if OS_MONITOR_DEADLINE
    /* Deadline monitoring — only flag the miss; PendSV handles action */
    if (current_task && current_task != &idle_tcb &&
        current_task->state == TaskState::RUNNING &&
        current_task->deadline_ticks > 0) {
        uint32_t elapsed = tick_count - current_task->last_yield_tick;
        if (elapsed > current_task->deadline_ticks) {
            current_task->deadline_miss_count++;
            os_report_error(OSError::DEADLINE_MISS);
            /* Do NOT reset/disable task from ISR context —
               only PendSV (lowest priority) should modify current_task.
               The miss is recorded; higher-level code can react. */
        }
    }
#endif

    if (blocked_count > 0) {
        for (TCB* task = task_list; task; task = task->next) {
            if (task->state != TaskState::BLOCKED) continue;

            if (task->delay_ticks > 0) {
                task->delay_ticks--;
                if (task->delay_ticks == 0) {
                    os_wake_on_delay_expiry(task);
                }
            } else if (task->block_timeout > 0) {
                task->block_timeout--;
                if (task->block_timeout == 0) {
                    os_wake_on_timeout_expiry(task);
                }
            }
        }
    }

    /* Stack check mutates the same scheduler structures, so it stays
       inside the critical section too. */
    if ((tick_count & 0x0F) == 0) os_stack_check_all();

    os_critical_exit(cs);
    /* ═══════ end C2 fix ═══════ */

    /* DSB: all tick processing (task state changes, wakeups, stack checks)
       must be visible in RAM before PendSV runs and reads them. */
    __asm volatile("dsb" ::: "memory");
    /* Always fire PendSV — scheduler handles time-slicing,
       periodic task period, and round-robin rotation. */
    OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
}


/* ═══════════════ Tickless Idle ═════════════════════════════════ */
#if OS_TOOL_TICKLESS_IDLE
static bool os_idle_tickless(void) {
    uint32_t saved_rvr = os_syst_rvr_normal;
    if (saved_rvr == 0) return false;

    uint32_t ticks_per_os_tick = saved_rvr + 1;
    uint32_t sleep = 0;
    uint32_t hw_sleep = 0;

    {
        uint32_t cs = os_critical_enter();
        OS_SYST_CSR = 0;

        uint32_t max_sleep = 0x00FFFFFFUL / ticks_per_os_tick;
        if (max_sleep == 0) max_sleep = 1;
        sleep = max_sleep;

        if (blocked_count > 0) {
            for (TCB* t = task_list; t; t = t->next) {
                if (t->state == TaskState::BLOCKED) {
                    if (t->delay_ticks > 0 && t->delay_ticks < sleep)
                        sleep = t->delay_ticks;
                    if (t->block_timeout > 0 && t->block_timeout < sleep)
                        sleep = t->block_timeout;
                }
            }
        }

        if (sleep <= 1) {
            OS_SYST_CVR = 0;
            OS_SYST_RVR = saved_rvr;
            OS_SYST_CSR = 0x07;
            os_critical_exit(cs);
            return false;
        }

        uint64_t hw64 = (uint64_t)ticks_per_os_tick * sleep;
        if (hw64 > 0x00FFFFFFULL) {
            sleep = 0x00FFFFFFUL / ticks_per_os_tick;
            if (sleep <= 1) {
                OS_SYST_CVR = 0;
                OS_SYST_RVR = saved_rvr;
                OS_SYST_CSR = 0x07;
                os_critical_exit(cs);
                return false;
            }
            hw64 = (uint64_t)ticks_per_os_tick * sleep;
        }
        hw_sleep = (uint32_t)hw64;

        OS_SYST_CVR = 0;
        OS_SYST_RVR = hw_sleep - 1;
        OS_SYST_CSR = 0x07;
        os_critical_exit(cs);
    }

    __asm volatile("dsb" ::: "memory");
    __asm volatile("wfi");
    __asm volatile("isb" ::: "memory");

    {
        uint32_t cs = os_critical_enter();
        uint32_t cvr_val = OS_SYST_CVR;
        OS_SYST_CSR = 0;

        /* Always derive the elapsed time from the hardware counter, not
           COUNTFLAG. COUNTFLAG is sticky (stays set until CSR is read) and
           can be stale when a higher-priority ISR preempts the SysTick ISR.
           CVR reflects actual elapsed time regardless of ISR state. */
        if (cvr_val < hw_sleep) {
            uint32_t elapsed_hw = (hw_sleep - 1) - cvr_val;
            uint32_t skip = elapsed_hw / ticks_per_os_tick;
            if (skip > 0) {
                /* C3 fix: apply the skipped time right now, inside the CS —
                   forward tick_count and wake expired tasks. Deferring this
                   to the next SysTick loses time whenever another IRQ (e.g.
                   the HAL TIM1 time base) wakes us repeatedly before that
                   SysTick fires, because tick_skip would be overwritten. */
                idle_ticks += skip;
                if (os_tickless_process(skip)) {
                    /* A task expired while we slept — reschedule now */
                    OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
                }
            }
        }
        /* else: CVR >= hw_sleep means SysTick hasn't started counting
           down yet (or counter just reloaded). No time elapsed. */

        OS_SYST_CVR = 0;
        OS_SYST_RVR = saved_rvr;
        OS_SYST_CSR = 0x07;
        os_critical_exit(cs);
    }

    return true;
}
#endif /* OS_TOOL_TICKLESS_IDLE */

/* Forward declaration for tickless processing (in ZenOS_Scheduler.cpp) */
#if OS_TOOL_TICKLESS_IDLE
extern bool os_tickless_process(uint32_t skip);
#endif


/* ═══════════════ Idle ═══════════════ */
static void os_idle_task(void) {
    while (1) {
#if OS_SAFETY_HW_WATCHDOG
        os_hw_watchdog_check();  /* feed if healthy */
#endif
#if OS_SAFETY_CRC_CHECK
        os_crc_check_step();    /* check ROM in background */
#endif
#if OS_TOOL_TICKLESS_IDLE
        os_idle_tickless();
#endif
        __asm volatile("wfi");
    }
}

/* ═══════════════ Init ═══════════════ */
extern "C" void os_init(void) {
    os_event_next_id = 0;
    task_list = nullptr; task_count = 0; current_task = nullptr;
    tick_count = 0; blocked_count = 0;

    /* idle_tcb: safe defaults for fault recovery before os_start */
    idle_tcb.id = 255; idle_tcb.name = "idle"; idle_tcb.entry = nullptr;
    idle_tcb.priority = 0; idle_tcb.base_priority = 0;
    idle_tcb.state = TaskState::INACTIVE;
    idle_tcb.next = nullptr;
    os_safe_depth = 0; error_total = 0; error_expected = 0;
    error_expect_depth = 0; error_last = OSError::NONE;
    idle_ticks = 0; wdg_reset_count = 0; stack_recovery_count = 0;
    os_syst_rvr_normal = 0;
    /* O(1) scheduler: initialize priority bitmap and queues */
    os_ready_bitmap = 0;
    for (uint32_t i = 0; i < 32; i++) os_pq_head[i] = nullptr;


#if OS_HAS_CYCLE_COUNTER
    OS_COREDEBUG_DEMCR |= OS_COREDEM_TRCENA;
    OS_DWT_CYCCNT = 0;
    OS_DWT_CTRL  |= OS_DWT_CYCCNTENA;
#endif
#if OS_SAFETY_MPU
    os_mpu_init();
#endif
}

/* ═══════════════ Start ═══════════════ */
extern "C" void os_start(void) {
    idle_tcb.id = 255; idle_tcb.name = "idle"; idle_tcb.entry = os_idle_task;
    idle_tcb.priority = 0; idle_tcb.state = TaskState::READY;
    idle_tcb.stack_base = idle_stack; idle_tcb.stack_size = 64;
    idle_tcb.period_ticks = 0; idle_tcb.next_run_time = 0;
    idle_tcb.delay_ticks = 0; idle_tcb.blocking_on = nullptr;
    idle_tcb.block_timeout = 0; idle_tcb.wait_result = 0;
    idle_tcb.last_yield_tick = tick_count;
    idle_tcb.mutex_nesting = 0; idle_tcb.base_priority = 0;
    idle_tcb.next = task_list;
    task_list = &idle_tcb;
    os_stack_init(&idle_tcb);
    /* O(1) scheduler: add idle to priority queue (prio=0) */
    os_pq_add(&idle_tcb);


    {
        uint32_t* fv = (uint32_t*)OS_SCB_VTOR;
        for (uint32_t i = 0; i < OS_VECTOR_COUNT; i++)
            ram_vectors[i] = fv[i];

        ram_vectors[OS_PENDSV_VECTOR_INDEX]     = (uint32_t)OS_PendSV_Handler;
        ram_vectors[OS_HARDFAULT_VECTOR_INDEX]  = (uint32_t)OS_Fault_Handler;
        ram_vectors[OS_MEMMANAGE_VECTOR_INDEX]  = (uint32_t)OS_Fault_Handler;
        ram_vectors[OS_BUSFAULT_VECTOR_INDEX]   = (uint32_t)OS_Fault_Handler;
        ram_vectors[OS_USAGEFAULT_VECTOR_INDEX] = (uint32_t)OS_Fault_Handler;
        OS_SCB_VTOR = (uint32_t)ram_vectors;
    }

    OS_SYST_CSR = 0; OS_SYST_CVR = 0;
    uint32_t reload = (SystemCoreClock / 1000000UL) * OS_KERNEL_TICK_PERIOD_US;
    if (reload == 0) reload = 1;
    if (reload > 0x00FFFFFFUL) reload = 0x00FFFFFFUL;
    os_syst_rvr_normal = reload - 1;
    OS_SYST_RVR = reload - 1;

    OS_PENDSV_PRIO  = 0xFE;
    OS_SYSTICK_PRIO = 0xFF;

    /* PendSV at 0xFE is above all common IRQ priorities (0, 5, 0xFF)
       No conflict check needed — PendSV always preempts them. */

    {
        uint32_t msp_val = (uint32_t)(fault_stack + 48);
        msp_val &= ~7UL;
        __asm volatile("msr msp, %0" :: "r"(msp_val));
    }

    __asm volatile("msr psp, %0" :: "r"(idle_tcb.stack_top));
    __asm volatile("msr control, %0" :: "r"(0x02));
    __asm volatile("isb");

    OS_SYST_CSR = 0x07;
    OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    os_hw_enable_irq();
    os_started = true;

    while (1) __asm volatile("wfi");
}

/* ═══════════════ PendSV Handler — O(1) Bitmap Scheduler ═══════════════
   Uses os_pq_next() with CLZ to find highest priority task in O(1).
   The old O(n) linked list scan is replaced by a bitmap lookup. ═══════════════ */
extern "C" OS_NAKED OS_USED void OS_PendSV_Handler(void) {
    __asm volatile(
        ".syntax unified\n"
        ".thumb\n"
        "mrs r0, psp\n"
        "ldr r1, =current_task\n"
        "ldr r1, [r1]\n"
        "cmp r1, #0\n"
        "bne save_ctx\n"
        "b first_run\n"

        "save_ctx:\n"
        "stmdb r0!, {r4-r11}\n"
        "str r0, [r1, #" OS_STR(OS_OFF_STACK_TOP) "]\n"

        "ldrb r2, [r1, #" OS_STR(OS_OFF_STATE) "]\n"
        "cmp r2, #2\n"
        "bne skip_save\n"
        "movs r2, #1\n"
        "strb r2, [r1, #" OS_STR(OS_OFF_STATE) "]\n"
        "skip_save:\n"

        /* Load r8/r9 AFTER saving context so the interrupted task's
           original register values are preserved on its stack. */
        "ldr r8, =tick_count\n"
        "ldr r9, =os_idle_tcb_ptr\n"

        /* O(1) Scheduler: call os_pq_next() to get highest priority task.
           r8/r9 hold tick_count/os_idle_tcb_ptr addresses across the calls. */
        "push {lr}\n"
        "bl os_pq_next\n"
        "mov r5, r0\n"
        "bl os_pq_rotate\n"
        "pop {lr}\n"

        "cmp r5, #0\n"
        "beq fallback_idle_sched\n"

        "ldr r1, =current_task\n"
        "str r5, [r1]\n"
        "movs r6, #2\n"
        "strb r6, [r5, #" OS_STR(OS_OFF_STATE) "]\n"
        "ldr r7, [r5, #" OS_STR(OS_OFF_PERIOD_TICKS) "]\n"
        "cmp r7, #0\n"
        "beq ts_no_period\n"
        "ldr r6, [r8]\n"
        "adds r6, r6, r7\n"
        "str r6, [r5, #" OS_STR(OS_OFF_NEXT_RUN_TIME) "]\n"
        "b ts_done\n"
        "ts_no_period:\n"
        "ldr r6, [r8]\n"
        "adds r6, r6, #1\n"
        "str r6, [r5, #" OS_STR(OS_OFF_NEXT_RUN_TIME) "]\n"
        "ts_done:\n"
        "ldr r0, [r5, #" OS_STR(OS_OFF_STACK_TOP) "]\n"
        "b restore_ctx\n"

        "fallback_idle_sched:\n"
        "ldr r5, [r9]\n"
        "ldr r1, =current_task\n"
        "str r5, [r1]\n"
        "movs r6, #2\n"
        "strb r6, [r5, #" OS_STR(OS_OFF_STATE) "]\n"
        "ldr r6, [r8]\n"
        "adds r6, r6, #1\n"
        "str r6, [r5, #" OS_STR(OS_OFF_NEXT_RUN_TIME) "]\n"
        "ldr r0, [r5, #" OS_STR(OS_OFF_STACK_TOP) "]\n"
        "b restore_ctx\n"

        "first_run:\n"
        "ldr r8, =tick_count\n"
        "ldr r9, =os_idle_tcb_ptr\n"
        "push {lr}\n"
        "bl os_pq_next\n"
        "mov r5, r0\n"
        "bl os_pq_rotate\n"
        "pop {lr}\n"

        "cmp r5, #0\n"
        "beq fallback_idle_first\n"
        "ldr r1, =current_task\n"
        "str r5, [r1]\n"
        "movs r6, #2\n"
        "strb r6, [r5, #" OS_STR(OS_OFF_STATE) "]\n"
        "ldr r7, [r5, #" OS_STR(OS_OFF_PERIOD_TICKS) "]\n"

        "cmp r7, #0\n"
        "beq first_ts_no_period\n"
        "ldr r6, [r8]\n"
        "adds r6, r6, r7\n"
        "str r6, [r5, #" OS_STR(OS_OFF_NEXT_RUN_TIME) "]\n"
        "b first_ts_done\n"
        "first_ts_no_period:\n"
        "ldr r6, [r8]\n"
        "adds r6, r6, #1\n"
        "str r6, [r5, #" OS_STR(OS_OFF_NEXT_RUN_TIME) "]\n"
        "first_ts_done:\n"
        "ldr r0, [r5, #" OS_STR(OS_OFF_STACK_TOP) "]\n"
        "b restore_ctx\n"

        "fallback_idle_first:\n"
        "ldr r5, [r9]\n"
        "ldr r1, =current_task\n"
        "str r5, [r1]\n"
        "movs r6, #2\n"
        "strb r6, [r5, #" OS_STR(OS_OFF_STATE) "]\n"
        "ldr r0, [r5, #" OS_STR(OS_OFF_STACK_TOP) "]\n"
#if OS_SAFETY_MPU
        /* ── MPU per-task switch (next task in r5) ──
           Tasks run unprivileged (CONTROL.nPRIV=1); the idle task stays
           privileged so it can reach everything (watchdog, CRC, MPU). */
        "ldr   r6, [r9]\n"
        "cmp   r5, r6\n"
        "beq   3f\n"
        "mrs   r6, CONTROL\n"
        "orr   r6, r6, #0x01\n"
        "msr   CONTROL, r6\n"
        "isb\n"
        "push  {r0-r3, lr}\n"
        "mov   r0, r5\n"
        "bl    os_mpu_configure_task\n"
        "pop   {r0-r3, lr}\n"
        "b     4f\n"
        "3:\n"
        "mrs   r6, CONTROL\n"
        "bic   r6, r6, #0x01\n"
        "msr   CONTROL, r6\n"
        "isb\n"
        "4:\n"
#endif

        "restore_ctx:\n"
        "ldmia r0!, {r4-r11}\n"
        "msr psp, r0\n"
        "isb\n"
        "bx lr\n"
        /* Flush literal pool here — keeps ldr rX,=sym offsets within ±4KB */
        ".ltorg\n"
    );
}
