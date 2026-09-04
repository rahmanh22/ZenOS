/**
 * @file    ZenOS_Monitor.cpp
 * @brief   ZenOS RTOS — Runtime Monitoring: Stack Watermark, CPU Usage,
 *          Deadline, Error Log Queries
 *
 * Extracted from ZenOS.cpp as part of the modular split.
 *
 * @author  Rahman Heidari <rahman.h22@gmail.com> — Raymon Research Team
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS_Internal.hpp"

/* ═══════════════ Stack Watermarking ═══════════════
   Canary pattern: 0xA5A5A5A5 (set in os_stack_init).
   Scan from stack_base upward to find first non-canary word = peak usage. ═══════════════ */
#if OS_MONITOR_ENABLED
uint32_t os_stack_watermark_scan(const TCB* task) {
    if (!task || !task->stack_base || task->stack_size == 0) return 0;
    const uint32_t* base = task->stack_base;
    uint32_t size = task->stack_size;
    uint32_t used_words = 0;
    /* The canary region (base[0..CANARY_COUNT-1], 0xDEADBEEF) sits at the
       bottom of the stack — skip it and scan for the first non-fill word,
       which marks the deepest the stack pointer has ever reached. */
    for (uint32_t i = OS_STACK_CANARY_COUNT; i < size; i++) {
        if (base[i] != 0xA5A5A5A5UL) {
            used_words = size - i;
            break;
        }
    }
    return used_words * 4; /* Return bytes used */
}

extern "C" uint32_t os_get_stack_watermark(uint8_t task_id) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_id(task_id);
    if (!t) { os_critical_exit(cs); return 0; }
    uint32_t used = os_stack_watermark_scan(t);
    os_critical_exit(cs);
    return used;
}

extern "C" uint32_t os_get_stack_watermark_percent(uint8_t task_id) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_id(task_id);
    if (!t) { os_critical_exit(cs); return 0; }
    uint32_t used = os_stack_watermark_scan(t);
    uint32_t total = t->stack_size * 4;
    os_critical_exit(cs);
    return (total > 0) ? (used * 100 / total) : 0;
}

/* ── Per-task stack usage report (enumerate all tasks by index) ── */
extern "C" uint8_t os_get_stack_report_count(void) {
    uint8_t n = 0;
    uint32_t cs = os_critical_enter();
    for (TCB* t = task_list; t; t = t->next)
        if (t->stack_base) n++;
    os_critical_exit(cs);
    return n;
}

extern "C" bool os_get_stack_report(uint8_t index, os_stack_report_entry_t* out) {
    if (!out) return false;
    uint32_t cs = os_critical_enter();
    uint8_t i = 0;
    for (TCB* t = task_list; t; t = t->next) {
        if (!t->stack_base) continue;
        if (i == index) {
            out->name       = t->name;
            out->id         = t->id;
            out->size_bytes = t->stack_size * 4;
            out->peak_bytes = os_stack_watermark_scan(t);
            os_critical_exit(cs);
            return true;
        }
        i++;
    }
    os_critical_exit(cs);
    return false;
}
#endif /* OS_MONITOR_ENABLED */


/* ═══════════════ CPU Usage ═══════════════ */
#if OS_MONITOR_ENABLED
extern "C" uint8_t os_get_cpu_usage(void) {
    static uint32_t lt = 0, li = 0;
    uint32_t t = tick_count, i = idle_ticks;
    uint32_t dt = t - lt, di = i - li;
    lt = t; li = i;
    if (dt == 0) return 0;
    uint32_t u = 100UL - (uint32_t)(((uint64_t)di * 100UL) / dt);
    return (u > 100) ? 100 : (uint8_t)u;
}

extern "C" uint8_t os_get_cpu_usage_total(void) {
    uint32_t t = tick_count;
    uint32_t i = idle_ticks;
    if (t == 0) return 0;
    uint32_t u = 100UL - (uint32_t)(((uint64_t)i * 100UL) / t);
    return (u > 100) ? 100 : (uint8_t)u;
}

extern "C" uint32_t os_get_stack_usage(void(*entry)(void)) {
    TCB* t = os_find_task_by_entry(entry);
    if (!t || !t->stack_base) return 0;
    return os_stack_watermark_scan(t);
}

extern "C" uint8_t os_get_task_cpu_usage(void(*entry)(void)) {
    TCB* t = os_find_task_by_entry(entry);
    if (!t || t == &idle_tcb) return 0;
    uint32_t total = tick_count;
    if (total == 0) return 0;
    uint32_t percent = (t->cpu_ticks * 100UL) / total;
    return (percent > 100) ? 100 : (uint8_t)percent;
}
#endif /* OS_MONITOR_ENABLED */

/* ═══════════════ Deadline ═══════════════ */
#if OS_MONITOR_DEADLINE
extern "C" void os_task_set_deadline_raw(void(*entry)(void), uint32_t deadline_ms) {
    TCB* t = os_find_task_by_entry(entry);
    if (!t) return;
    /* deadline_ms == 0 disables the deadline. os_ms_to_ticks(0) would
       otherwise return 1 (its "at least one tick" guard), turning a reset
       into a 1-tick deadline that streams DEADLINE_MISS on every tick. */
    uint32_t deadline_ticks = (deadline_ms == 0) ? 0 : os_ms_to_ticks(deadline_ms);
    uint32_t cs = os_critical_enter();
    t->deadline_ticks = deadline_ticks;
    t->deadline_miss_count = 0;
    t->last_yield_tick = tick_count;
    os_critical_exit(cs);
}

extern "C" uint32_t os_get_deadline_miss_count(void(*entry)(void)) {
    uint32_t cs = os_critical_enter();
    TCB* t = os_find_task_by_entry(entry);
    uint32_t result = t ? t->deadline_miss_count : 0;
    os_critical_exit(cs);
    return result;
}
#endif
