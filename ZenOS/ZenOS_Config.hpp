#pragma once
/**
 * @file    ZenOS_Config.hpp
 * @brief   ZenOS RTOS Configuration
 *
 * This is the ONLY file users modify for behavior settings.
 * Resource sizes are calculated automatically.
 * Hardware detection is in ZenOS_Port.hpp.
 *
 * Naming convention:
 *   OS_KERNEL_*   — core timing and memory
 *   OS_TOOL_*     — scheduling and communication primitives
 *   OS_MONITOR_*  — runtime observability
 *   OS_SAFETY_*   — fault detection and hardware protection
 *
 * @author  Rahman Heidari (rahman.h22@gmail.com)
 * @version 1.0.0
 */


/* ============================================================================
 *  OS_KERNEL — Core Timing and Memory
 * ============================================================================ */

/* Tick resolution in microseconds (100..1000, must divide 1000 evenly) */
#ifndef OS_KERNEL_TICK_PERIOD_US
#define OS_KERNEL_TICK_PERIOD_US  100UL
#endif

/* Default stack size per task in bytes */
#ifndef OS_KERNEL_STACK_SIZE
#define OS_KERNEL_STACK_SIZE  256
#endif


/* ============================================================================
 *  OS_TOOL — Scheduling and Communication Primitives
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* Inter-task signaling (binary/counting events) */
#ifndef OS_TOOL_EVENT
#define OS_TOOL_EVENT          1
#endif

/* Mutual exclusion with priority inheritance */
#ifndef OS_TOOL_MUTEX
#define OS_TOOL_MUTEX          1
#endif

/* Bounded FIFO queue (template-based, compile-time capacity) */
#ifndef OS_TOOL_QUEUE
#define OS_TOOL_QUEUE          1
#endif

/* Counting semaphore with max-count */
#ifndef OS_TOOL_SEMAPHORE
#define OS_TOOL_SEMAPHORE      1
#endif

/* Tickless idle: WFI-based sleep for power savings */
#ifndef OS_TOOL_TICKLESS_IDLE
#define OS_TOOL_TICKLESS_IDLE  1
#endif


/* ============================================================================
 *  OS_MONITOR — Runtime Observability
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* Deadline monitoring: detect and act on task deadline misses */
#ifndef OS_MONITOR_DEADLINE
#define OS_MONITOR_DEADLINE    1
#endif

/* TCB integrity: magic-number check for memory corruption detection */
#ifndef OS_MONITOR_TCB_INTEGRITY
#define OS_MONITOR_TCB_INTEGRITY  1
#endif

/* Error log: circular buffer recording errors with timestamp and task ID */
#ifndef OS_MONITOR_ERROR_LOG
#define OS_MONITOR_ERROR_LOG   1
#endif

/* Error log capacity (number of entries) */
#ifndef OS_MONITOR_ERROR_LOG_SIZE
#define OS_MONITOR_ERROR_LOG_SIZE  32
#endif


/* ============================================================================
 *  OS_SAFETY — Fault Detection and Hardware Protection
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* RAM test: background March C- algorithm for SRAM integrity */
#ifndef OS_SAFETY_RAM_TEST
#define OS_SAFETY_RAM_TEST     1
#endif

/* MPU: hardware memory protection per task (Cortex-M3/M4/M7) */
#ifndef OS_SAFETY_MPU
#define OS_SAFETY_MPU          1
#endif

/* Hardware watchdog feed/check integration */
#ifndef OS_SAFETY_HW_WATCHDOG
#define OS_SAFETY_HW_WATCHDOG  1
#endif

/* CRC check: ROM integrity verification via CRC peripheral */
#ifndef OS_SAFETY_CRC_CHECK
#define OS_SAFETY_CRC_CHECK    1
#endif

/* Software watchdog: detect stuck tasks and recover */
#ifndef OS_SAFETY_SOFT_WATCHDOG
#define OS_SAFETY_SOFT_WATCHDOG  1
#endif

/* Action on deadline miss: 0 = log only, 1 = reset task, 2 = disable task */
#ifndef OS_SAFETY_DEADLINE_ACTION
#define OS_SAFETY_DEADLINE_ACTION  1
#endif

/* Max task recovery attempts before permanent disable */
#ifndef OS_SAFETY_TASK_MAX_RECOVERY
#define OS_SAFETY_TASK_MAX_RECOVERY  3
#endif

/* Software watchdog timeout in milliseconds */
#ifndef OS_SAFETY_SOFT_WDG_TIMEOUT_MS
#define OS_SAFETY_SOFT_WDG_TIMEOUT_MS  3000UL
#endif

/* Max duration (us) allowed inside OS_SAFE before reporting error */
#ifndef OS_SAFETY_MAX_CRITICAL_US
#define OS_SAFETY_MAX_CRITICAL_US  1000UL
#endif

/* ============================================================================
 *  Compile-Time Validation
 * ============================================================================ */

#if (OS_KERNEL_TICK_PERIOD_US < 100UL) || (OS_KERNEL_TICK_PERIOD_US > 1000UL)
#error "OS_KERNEL_TICK_PERIOD_US must be 100..1000"
#endif

#if (1000UL % OS_KERNEL_TICK_PERIOD_US) != 0
#error "OS_KERNEL_TICK_PERIOD_US must divide 1000 evenly"
#endif

#if (OS_SAFETY_DEADLINE_ACTION < 0) || (OS_SAFETY_DEADLINE_ACTION > 2)
#error "OS_SAFETY_DEADLINE_ACTION must be 0, 1, or 2"
#endif
