#ifndef ZenOS_C_H
#define ZenOS_C_H
/**
 * @file    ZenOS_c.h
 * @brief   Thin C wrapper for ZenOS — use from .c files only
 *
 * @author  Raymon Research Group (rahman.h22@gmail.com)
 * @version 1.0.0
 *
 * Declares every ZenOS function callable from C. For C++ code include
 * ZenOS.hpp directly (C++-only features: task creation templates, RAII
 * guards OS_SAFE/OS_LOCK, OS_EVENT, OS_MUTEX, OS_QUEUE, OS_SEMAPHORE,
 * and OSError-based reporting).
 *
 * Pulls in ZenOS_Config.hpp so the optional feature declarations below
 * match your build configuration automatically.
 *
 * Usage in .c files:
 *     #include "ZenOS_c.h"
 *     void SysTick_Handler(void) { os_tick(); }
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "ZenOS_Config.hpp"

/* ═══════════════ Core (always available) ═══════════════ */

/* Lifecycle & ISR entry */
void os_init(void);
void os_start(void);
void os_tick(void);

/* Critical sections */
uint32_t os_critical_enter(void);
void     os_critical_exit(uint32_t old);

/* Delay & yield (blocking calls are rejected from ISR context) */
void os_delay_ms(uint32_t ms);
void os_delay_us(uint32_t us);
void os_yield(void);

/* Task control (tasks are created from C++ via os_task_create) */
void     os_task_stop(void(*entry)(void));
void     os_task_start(void(*entry)(void));
uint16_t os_get_task_count(void);
bool     os_task_isActive(void(*entry)(void));

/* Time */
uint32_t os_get_tick(void);
uint32_t os_get_us(void);
uint32_t os_get_ms(void);

/* Error counters */
uint32_t os_get_error_count(void);
uint32_t os_in_safe(void);

/* Version */
uint32_t    os_get_version(void);
const char* os_get_version_string(void);

/* ISR handlers (used by the vector table) */
void OS_PendSV_Handler(void);
void OS_Fault_Handler(void);
void OS_Fault_C_Handler(void);


/* ═══════════════ Optional feature blocks ═══════════════
   Mirrors the switches in ZenOS_Config.hpp — each block compiles
   only when its feature is enabled. */

/* Monitor: task state / CPU usage / stack watermark */
#if OS_MONITOR_DEADLINE || OS_MONITOR_TCB_INTEGRITY || OS_MONITOR_ERROR_LOG
uint8_t  os_task_get_state(void(*entry)(void));
uint8_t  os_get_cpu_usage(void);
uint8_t  os_get_cpu_usage_total(void);
uint32_t os_get_wdg_reset_count(void);
uint32_t os_get_stack_recovery_count(void);
uint32_t os_get_stack_usage(void(*entry)(void));
uint8_t  os_get_task_cpu_usage(void(*entry)(void));
uint32_t os_get_stack_watermark(uint8_t task_id);
uint32_t os_get_stack_watermark_percent(uint8_t task_id);
#endif

/* Deadline monitoring */
#if OS_MONITOR_DEADLINE
void     os_task_set_deadline_raw(void(*entry)(void), uint32_t deadline_ms);
uint32_t os_get_deadline_miss_count(void(*entry)(void));
#endif

/* Hardware watchdog (IWDG) */
#if OS_SAFETY_HW_WATCHDOG
void     os_hw_watchdog_feed(void);
void     os_hw_watchdog_check(void);
uint32_t os_get_hw_wdg_reset_count(void);
#endif

/* Background RAM test (March-C) */
#if OS_SAFETY_RAM_TEST
void     os_ram_test_step(void);
uint8_t  os_ram_test_progress(void);
uint32_t os_get_ram_test_error_count(void);
bool     os_ram_test_complete(void);
#endif

/* MPU protection */
#if OS_SAFETY_MPU
void os_mpu_init(void);
void os_mpu_configure_task(void* tcb_ptr);
void os_mpu_add_region(void* tcb_ptr, uint32_t base, uint32_t size, uint32_t attrs);
void os_mpu_disable(void);
void os_mpu_enable(void);
#endif

/* ROM CRC integrity check */
#if OS_SAFETY_CRC_CHECK
void     os_crc_init(void);
void     os_crc_check_step(void);
uint8_t  os_crc_check_progress(void);
bool     os_crc_check_complete(void);
uint32_t os_get_crc_error_count(void);
#endif

/* SMP (multi-core) */
#if OS_SMP_CORES > 1
uint8_t os_get_core_id(void);
uint8_t os_get_core_count(void);
void    os_task_set_core(void(*entry)(void), uint8_t core);
void    os_task_migrate(void(*entry)(void), uint8_t core);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZenOS_C_H */
