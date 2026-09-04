#pragma once
/**
 * @file    ZenOS_Internal.hpp
 * @brief   ZenOS RTOS — Internal shared declarations between modules
 *
 * This header is NOT part of the public API. It provides extern
 * declarations for functions and globals shared across the split
 * translation units (scheduler, IPC, safety, monitor).
 *
 * @author  Raymon Research Group (rahman.h22@gmail.com)
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS.hpp"

/* ═══════════════ Shared Globals ═══════════════ */
extern TCB*              volatile task_list;
extern uint16_t          volatile task_count;
extern TCB*              volatile current_task;
extern volatile uint32_t tick_count;
extern volatile uint32_t os_safe_depth;

extern uint32_t          idle_stack[];
extern TCB               idle_tcb;
extern "C" TCB* const    os_idle_tcb_ptr;

extern volatile uint32_t blocked_count;
extern volatile uint32_t idle_ticks;  /* defined in ZenOS.cpp */
extern volatile bool     os_started;

extern volatile uint32_t error_total;
extern volatile OSError  error_last;
extern volatile uint32_t error_expected;
extern volatile uint32_t error_expect_depth;
extern volatile uint32_t wdg_reset_count;
extern volatile uint32_t stack_recovery_count;

extern volatile uint32_t os_ready_bitmap;
extern TCB* volatile     os_pq_head[32];

extern volatile uint32_t os_syst_rvr_normal;

#if OS_TOOL_EVENT
extern ECB* volatile     event_list;
extern int16_t           os_event_next_id;
#endif

/* ═══════════════ Scheduler Functions ═══════════════ */
void os_pq_add(TCB* task);
void os_pq_remove(TCB* task);

void os_stack_init(TCB* task);
void os_reset_task_internal(TCB* task);
void os_task_exit(void);

void os_wake_task(TCB* t, uint8_t result);
bool os_block_current(TaskState state, void* blocking_on,
                      uint32_t block_timeout, uint32_t delay_ticks);

inline void os_wake_on_delay_expiry(TCB* task) {
    os_wake_task(task, 1);
}

inline void os_wake_on_timeout_expiry(TCB* task) {
    task->wait_result = 2;
    os_wake_task(task, 2);
}

uint32_t os_ms_to_ticks(uint32_t ms);
TCB* os_find_task_by_entry(void(*entry)(void));
TCB* os_find_task_by_id(uint8_t id);

#if OS_TOOL_TICKLESS_IDLE
bool os_tickless_process(uint32_t skip);
#endif

#if OS_TOOL_TICKLESS_IDLE
bool os_tickless_process(uint32_t skip);
#endif

/* ═══════════════ ISR Detection ═══════════════ */
inline bool os_in_isr(void) {
    uint32_t ipsr;
    __asm volatile("mrs %0, IPSR" : "=r"(ipsr));
    return ipsr != 0;
}

/* ═══════════════ Error Reporting ═══════════════ */
void os_report_error(OSError code);

/* ═══════════════ Safety Functions ═══════════════ */
void os_stack_check_all(void);

#if OS_MONITOR_TCB_INTEGRITY
inline bool os_tcb_check_magic(TCB* t) {
    return t && t->magic == OS_TCB_MAGIC;
}
#endif

/* ═══════════════ Monitor Functions ═══════════════ */
uint32_t os_stack_watermark_scan(const TCB* task);

/* ═══════════════ Critical Section ═══════════════ */
extern "C" uint32_t os_critical_enter(void);
extern "C" void os_critical_exit(uint32_t old);
