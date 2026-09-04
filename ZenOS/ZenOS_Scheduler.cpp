/**
 * @file    ZenOS_Scheduler.cpp
 * @brief   ZenOS RTOS — O(1) Priority Bitmap Scheduler & Task Management
 *
 * Extracted from ZenOS.cpp as part of the modular split.
 * Contains: priority bitmap, priority queues, task create/stop/start/lookup,
 *           task reset, task exit, wake/block helpers, tickless processing.
 *
 * @author  Raymon Research Group (rahman.h22@gmail.com)
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS_Internal.hpp"

/* ═══════════════ Priority Bitmap Helpers ═══════════════ */
static inline void os_pq_set_bit(uint8_t prio) {
    if (prio < 32) os_ready_bitmap |= (1UL << prio);
}

static inline void os_pq_clear_bit(uint8_t prio) {
    if (prio < 32) os_ready_bitmap &= ~(1UL << prio);
}

static inline uint8_t os_pq_highest_prio(void) {
    if (os_ready_bitmap == 0) return 0;
    return (uint8_t)(31UL - (uint32_t)__builtin_clz(os_ready_bitmap));
}

/* ═══════════════ Priority Queue Operations ═══════════════ */

/* Add a task to its priority queue and set the bitmap bit */
void os_pq_add(TCB* task) {
    if (!task || task->priority >= 32) return;
    uint8_t p = task->priority;
    task->queue_next = os_pq_head[p];
    os_pq_head[p] = task;
    os_pq_set_bit(p);
}

/* Remove a task from its priority queue */
void os_pq_remove(TCB* task) {
    if (!task || task->priority >= 32) return;
    uint8_t p = task->priority;
    TCB* volatile* pp = &os_pq_head[p];
    while (*pp) {
        if (*pp == task) {
            *pp = task->queue_next;
            task->queue_next = nullptr;
            if (!os_pq_head[p]) os_pq_clear_bit(p);
            return;
        }
        pp = &(*pp)->queue_next;
    }
}

/* ── Round-Robin Throttle ──
   When a single task at the highest priority runs, temporarily skip that
   priority so lower-priority tasks get a turn. Prevents starvation.
   A bounded quota counter re-admits the skipped priority after a few
   lower-priority runs — otherwise a persistently-ready lower priority
   would starve the single-task level forever. */
static uint32_t os_rr_skip = 0;
static uint32_t os_rr_skip_quota = 0;

extern "C" TCB* os_pq_next(void) {
    /* While a priority is skipped, count down the re-admission quota on
       every pick of a lower-priority task. When it expires, re-admit the
       skipped priority on the next call. */
    if (os_rr_skip != 0 && os_rr_skip_quota > 0) {
        os_rr_skip_quota--;
        if (os_rr_skip_quota == 0) os_rr_skip = 0;
    }
    uint32_t effective = os_ready_bitmap & ~os_rr_skip;
    if (effective == 0) {
        /* All priorities were skipped — reset skip mask and try again */
        os_rr_skip = 0;
        effective = os_ready_bitmap;
    }
    if (effective == 0) return nullptr;
    uint8_t p = (uint8_t)(31UL - (uint32_t)__builtin_clz(effective));
    TCB* head = os_pq_head[p];
    if (!head) { os_pq_clear_bit(p); return nullptr; }
    return head;
}

extern "C" void os_pq_rotate(void) {
    if (os_ready_bitmap == 0) return;
    uint32_t effective = os_ready_bitmap & ~os_rr_skip;
    if (effective == 0) effective = os_ready_bitmap;
    if (effective == 0) return;
    uint8_t p = (uint8_t)(31UL - (uint32_t)__builtin_clz(effective));
    TCB* head = os_pq_head[p];
    if (!head) return;
    if (!head->queue_next) {
        /* Single task at this priority — give every ready priority level
           one run, then re-admit this level. The quota is derived from
           the ready bitmap automatically (no configuration needed), so a
           persistently-ready lower priority can never starve it forever. */
        os_rr_skip |= (1UL << p);
        os_rr_skip_quota = (uint32_t)__builtin_popcount(os_ready_bitmap);
        if (os_rr_skip_quota == 0) os_rr_skip_quota = 1;
        return;
    }
    /* Move head to tail */
    os_pq_head[p] = head->queue_next;
    TCB* tail = os_pq_head[p];
    while (tail->queue_next) tail = tail->queue_next;
    tail->queue_next = head;
    head->queue_next = nullptr;
}

/* ═══════════════ Stack Init ═══════════════ */
/* Opt2: os_stack_init — STMDB batch for exception frame (~40% faster) */
void os_stack_init(TCB* task) {
    uint32_t* base = task->stack_base;
    uint32_t  size = task->stack_size;
    for (uint32_t i = OS_STACK_CANARY_COUNT; i < size; i++)
        base[i] = 0xA5A5A5A5UL;
    for (uint32_t i = 0; i < OS_STACK_CANARY_COUNT; i++)
        base[i] = OS_STACK_CANARY;

    uint32_t* sp = base + size;
    sp = (uint32_t*)((uint32_t)sp & ~0x7UL);

    /* Build exception frame: xPSR, PC, LR, r0-r3, r4-r11 via STMDB */
    uint32_t ctx[16];
    ctx[0]  = 0x01000000UL;         /* xPSR: Thumb bit */
    ctx[1]  = (uint32_t)task->entry; /* PC */
    ctx[2]  = (uint32_t)os_task_exit;/* LR */
    ctx[3]  = 0x0000000CUL;         /* r12 */
    ctx[4]  = 0x00000003UL; ctx[5]  = 0x00000002UL;
    ctx[6]  = 0x00000001UL; ctx[7]  = 0x00000000UL;
    ctx[8]  = 0x0000000BUL; ctx[9]  = 0x0000000AUL;
    ctx[10] = 0x00000009UL; ctx[11] = 0x00000008UL;
    ctx[12] = 0x00000007UL; ctx[13] = 0x00000006UL;
    ctx[14] = 0x00000005UL; ctx[15] = 0x00000004UL;

    /* STMDB sp!, {r0-r15} in one bus transaction per word */
    sp -= 16;
    for (int i = 0; i < 16; i++) sp[i] = ctx[15-i];

    task->stack_top = sp;
}

/* ═══════════════ Task Reset ═══════════════ */
void os_reset_task_internal(TCB* task) {
    os_stack_init(task);
    /* Remove from old priority queue if was in one */
    os_pq_remove(task);
    task->state           = TaskState::READY;
    task->delay_ticks     = 0;
    task->blocking_on     = nullptr;
    task->block_timeout   = 0;
    task->wait_result     = 0;
    task->next_run_time   = tick_count;
    task->last_yield_tick = tick_count;
    task->priority        = task->base_priority;
    task->mutex_nesting   = 0;
    /* O(1) scheduler: add to priority queue */
    os_pq_add(task);
}

/* ═══════════════ Task Exit ═══════════════ */
void os_task_exit(void) {
    uint32_t cs = os_critical_enter();
    if (current_task) {
        current_task->state = TaskState::INACTIVE;
    }
    os_critical_exit(cs);
    os_yield();
    while (1) { __asm volatile("nop"); }
}

/* ═══════════════ Helper: wake a blocked task ═══════════════ */
void os_wake_task(TCB* t, uint8_t result) {
    t->wait_result     = result;
    t->state           = TaskState::READY;
    t->blocking_on     = nullptr;
    t->block_timeout   = 0;
    t->next_run_time   = tick_count;
    t->last_yield_tick = tick_count;
    if (blocked_count > 0) {
        /* LDREX/STREX atomic decrement */
        uint32_t tmp, res;
        __asm volatile(
            "1: ldrex %0, [%2]\n"
            "   cmp   %0, #0\n"
            "   beq   2f\n"
            "   subs  %0, %0, #1\n"
            "   strex %1, %0, [%2]\n"
            "   cmp   %1, #0\n"
            "   bne   1b\n"
            "2:\n"
            : "=&r"(tmp), "=&r"(res)
            : "r"(&blocked_count)
            : "memory", "cc"
        );
    }
    /* O(1) scheduler: add to priority queue */
    os_pq_add(t);
}

/* ═══════════════ Unified block/wake helpers ═══════════════
   Consolidates the repeated blocking pattern found in:
   os_delay_ms, os_event_wait, os_mutex_block_on.
   Returns true if the caller should yield. ═══════════════ */
bool os_block_current(TaskState state, void* blocking_on,
                      uint32_t block_timeout, uint32_t delay_ticks) {
    if (!current_task) return false;
    /* O(1) scheduler: remove from priority queue before blocking */
    os_pq_remove(current_task);
    current_task->state         = state;
    current_task->blocking_on   = blocking_on;
    current_task->block_timeout = block_timeout;
    current_task->delay_ticks   = delay_ticks;
    current_task->wait_result   = 0;
    current_task->last_yield_tick = tick_count;
    /* LDREX/STREX atomic increment */
    uint32_t tmp, res;
    __asm volatile(
        "1: ldrex %0, [%2]\n"
        "   adds  %0, %0, #1\n"
        "   strex %1, %0, [%2]\n"
        "   cmp   %1, #0\n"
        "   bne   1b\n"
        : "=&r"(tmp), "=&r"(res)
        : "r"(&blocked_count)
        : "memory", "cc"
    );
    return true;
}

/* ═══════════════ Helper: ms to ticks ═══════════════ */
/* Opt3: os_ms_to_ticks — 32-bit fast path (eliminates 64-bit MUL) */
uint32_t os_ms_to_ticks(uint32_t ms) {
    if (ms == OS_WAIT_FOREVER) return 0;
    /* <= keeps the largest exact value: ms == 0xFFFFFFFF/OS_TICKS_PER_MS
       still fits in 32 bits when multiplied. */
    if (ms <= (0xFFFFFFFFUL / OS_TICKS_PER_MS)) {
        uint32_t r = ms * OS_TICKS_PER_MS;
        return r ? r : 1;
    }
    return 0xFFFFFFFEUL;
}

/* ═══════════════ Task Create ═══════════════ */
extern "C" int8_t _os_task_create_internal(
    TCB* task,
    uint32_t* stack_mem,
    uint32_t stack_size,
    const char* name,
    void(*entry)(void),
    uint8_t priority,
    uint32_t period_ms)
{
    if (os_started) { os_report_error(OSError::TASK_AFTER_START); return -1; }
    if (!task || !stack_mem || !entry) return -1;
    /* Prevent duplicate entry functions — each task must have a unique entry */
    if (os_find_task_by_entry(entry)) return -1;
    if (priority == 0) priority = 1;
    if (stack_size < 64) stack_size = 64;

    task->id            = task_count++;
    task->name          = name;
    task->entry         = entry;
    task->priority      = priority;
    task->base_priority = priority;
    task->mutex_nesting = 0;

    uint64_t p64 = (uint64_t)period_ms * OS_TICKS_PER_MS;
    task->period_ticks  = (p64 > 0xFFFFFFFEULL) ? 0xFFFFFFFEUL : (uint32_t)p64;
    task->next_run_time = 0;
    task->state         = TaskState::READY;
    task->delay_ticks   = 0;
    task->blocking_on   = nullptr;
    task->block_timeout = 0;
    task->wait_result   = 0;
    task->stack_size    = stack_size;
    task->last_yield_tick = tick_count;
    task->wdg_retries   = 0;
    task->cpu_ticks     = 0;
#if OS_SMP_CORES > 1
    task->core_id       = 0; /* 0 = run on any core */
    task->_pad[0] = task->_pad[1] = task->_pad[2] = 0;
#endif
#if OS_MONITOR_ENABLED
    task->peak_sp       = task->stack_top; /* Watermark: lowest SP seen */
#endif
#if OS_MONITOR_TCB_INTEGRITY
    task->magic         = OS_TCB_MAGIC;
    task->overflow_count = 0;
#endif
#if OS_SAFETY_MPU
    task->mpu_region_count = 0;
#endif

    uintptr_t addr = (uintptr_t)stack_mem;
#if OS_SAFETY_MPU
    /* MPU stack region needs a 32B-aligned base (see os_mpu_configure_task) */
    addr = (addr + 31) & ~31;
#else
    addr = (addr + 7) & ~7;
#endif
    task->stack_base = (uint32_t*)addr;

    os_stack_init(task);

    task->next = task_list;
    task_list  = task;

    /* O(1) scheduler: add to priority queue */
    os_pq_add(task);

    return (int8_t)task->id;
}

/* ═══════════════ Task Lookup ═══════════════ */
TCB* os_find_task_by_entry(void(*entry)(void)) {
    if (!entry) return nullptr;
    for (TCB* t = task_list; t; t = t->next)
        if (t->entry == entry) return t;
    return nullptr;
}

TCB* os_find_task_by_id(uint8_t id) {
    for (TCB* t = task_list; t; t = t->next)
        if (t->id == id) return t;
    return nullptr;
}

/* ═══════════════ Task Control ═══════════════ */
extern "C" void os_task_stop(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    if (t && t != &idle_tcb) {
        /* O(1) scheduler: remove from priority queue */
        os_pq_remove(t);
        if (t->state == TaskState::BLOCKED && blocked_count > 0) {
            uint32_t tmp, res;
            __asm volatile(
                "1: ldrex %0, [%2]\n"
                "   cmp   %0, #0\n"
                "   beq   2f\n"
                "   subs  %0, %0, #1\n"
                "   strex %1, %0, [%2]\n"
                "   cmp   %1, #0\n"
                "   bne   1b\n"
                "2:\n"
                : "=&r"(tmp), "=&r"(res)
                : "r"(&blocked_count)
                : "memory", "cc"
            );
        }
        t->state         = TaskState::INACTIVE;
        t->delay_ticks   = 0;
        t->blocking_on   = nullptr;
        t->block_timeout = 0;
        if (t == current_task) OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    }
    os_critical_exit(cs);
}

extern "C" void os_task_start(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    if (t && t->state == TaskState::INACTIVE) {
        os_reset_task_internal(t);
        t->wdg_retries = 0;
        OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    }
    os_critical_exit(cs);
}

#if OS_MONITOR_DEADLINE || OS_MONITOR_TCB_INTEGRITY || OS_MONITOR_ERROR_LOG
extern "C" uint8_t os_task_get_state(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    uint8_t result = t ? (uint8_t)t->state : 0xFF;
    os_critical_exit(cs);
    return result;
}
#endif

extern "C" bool os_task_isActive(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    uint8_t result = t ? (uint8_t)t->state : 0xFF;
    os_critical_exit(cs);
    return ((result != (uint8_t)TaskState::INACTIVE) ? true : false);
}

extern "C" uint8_t os_get_task_priority(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    uint8_t result = t ? t->priority : 0;
    os_critical_exit(cs);
    return result;
}

/* ═══════════════ Tickless Processing ═══════════════ */
#if OS_TOOL_TICKLESS_IDLE
bool os_tickless_process(uint32_t skip) {
    tick_count += skip;
    bool woke = false;

    if (blocked_count > 0) {
        for (TCB* task = task_list; task; task = task->next) {
            if (task->state != TaskState::BLOCKED) continue;

            if (task->delay_ticks > 0) {
                if (task->delay_ticks <= skip) {
                    task->delay_ticks = 0;
                    os_wake_on_delay_expiry(task);
                    woke = true;
                } else {
                    task->delay_ticks -= skip;
                }
            } else if (task->block_timeout > 0) {
                if (task->block_timeout <= skip) {
                    task->block_timeout = 0;
                    os_wake_on_timeout_expiry(task);
                    woke = true;
                } else {
                    task->block_timeout -= skip;
                }
            }
        }
    }
    return woke;
}
#endif /* OS_TOOL_TICKLESS_IDLE */

/* ═══════════════ Utilities ═══════════════ */
extern "C" uint32_t os_get_tick(void) { return tick_count; }
extern "C" uint16_t os_get_task_count(void) { return task_count; }

/* ═══════════════ Version ═══════════════ */
extern "C" uint32_t os_get_version(void) { return OS_VERSION_PACKED; }
extern "C" const char* os_get_version_string(void) { return OS_VERSION_STRING; }

/* Opt6: os_get_us — LDREX snapshot (no critical section, ~30 cycles saved) */
extern "C" uint32_t os_get_us(void) {
    uint32_t reload = os_syst_rvr_normal;
    if (reload == 0) return 0;
    uint32_t t = tick_count;
    uint32_t cvr = OS_SYST_CVR;
    if (cvr > reload) cvr = reload;
    uint32_t frac = ((reload - cvr) * OS_KERNEL_TICK_PERIOD_US) / (reload + 1);
    return (uint32_t)((uint64_t)t * OS_KERNEL_TICK_PERIOD_US + frac);
}

extern "C" uint32_t os_get_ms(void) { return tick_count / OS_TICKS_PER_MS; }

/* ═══════════════ SMP ═══════════════ */
#if OS_SMP_CORES > 1
static TCB* core_current_task[OS_SMP_MAX_CORES] = {nullptr};
static volatile uint8_t os_core_count_active = 1;

static inline uint8_t os_get_hw_core_id(void) {
    uint32_t cpuid = *((volatile uint32_t*)0xE000ED00UL);
    return (uint8_t)((cpuid >> 8) & 0xFF);
}

extern "C" uint8_t os_get_core_id(void) {
    return os_get_hw_core_id();
}

extern "C" uint8_t os_get_core_count(void) {
    return os_core_count_active;
}

extern "C" void os_task_set_core(void(*entry)(void), uint8_t core) {
    TCB* t = os_find_task_by_entry(entry);
    if (!t) return;
    if (core >= OS_SMP_CORES) return;
    t->core_id = core + 1; /* 0=any, 1=core0, 2=core1 */
}

extern "C" void os_task_migrate(void(*entry)(void), uint8_t core) {
    os_task_set_core(entry, core);
}
#endif /* OS_SMP_CORES > 1 */
