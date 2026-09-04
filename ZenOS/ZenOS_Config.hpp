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
 * ── Safety Standard Annotations ──────────────────────────────────
 * Each config option is annotated with the safety standards it
 * relates to.  Two macros at the bottom of this file enable
 * compile-time enforcement of mandatory requirements:
 *
 *   OS_TARGET_MEDICAL  — IEC 62304 (SW lifecycle) + IEC 60601-1 (medical EE)
 *                        Software safety classes:  A (lowest risk)
 *                                                  B (non-serious injury)
 *                                                  C (death or serious injury)
 *   OS_TARGET_INDUSTRIAL — IEC 61508 (functional safety) + IEC 61511 (process)
 *                          SIL levels: 1 (lowest) … 4 (highest)
 *
 * Legend for per-option annotations:
 *   [MED-A]  Medical IEC 62304 Class A — all options recommended
 *   [MED-B]  Medical IEC 62304 Class B — fault detection required
 *   [MED-C]  Medical IEC 62304 Class C — fault detection + tolerance required
 *   [IND-1]  Industrial IEC 61508 SIL 1 — basic fault detection
 *   [IND-2]  Industrial IEC 61508 SIL 2 — robust fault detection
 *   [IND-3]  Industrial IEC 61508 SIL 3 — high-integrity fault tolerance
 *   [IND-4]  Industrial IEC 61508 SIL 4 — highest-integrity (rare in SW)
 *
 * @author  Rahman Heidari <rahman.h22@gmail.com> — Raymon Research Team
 * @version 1.0.0
 */


/* ============================================================================
 *  OS_KERNEL — Core Timing and Memory
 * ============================================================================ */

/* Tick resolution in microseconds (100..1000, must divide 1000 evenly)
 * [MED-B] [MED-C] [IND-1] [IND-2] Must be ≤ 1 ms for Class B/C and SIL ≥ 1.
 *          Medical: IEC 62304 requires timing analysis for Class B/C;
 *          a tick ≤ 1 ms is necessary for deadline verification.
 *          Industrial: IEC 61508 Part 3 Table 4 — SW response time
 *          must be demonstrably less than the process safety time. */
#ifndef OS_KERNEL_TICK_PERIOD_US
#define OS_KERNEL_TICK_PERIOD_US  100UL
#endif

/* Compiler optimization level string for the banner.
   GCC's __OPTIMIZE__ macro is 1 for ALL levels >= O1 (O1, O2, O3, Og, Os),
   so it cannot distinguish between them.  Set this manually in your
   project's preprocessor defines to get the correct banner:
       -DOS_OPT_LEVEL_STRING="O2"
   If not defined, the banner shows "O+" for any optimized build.
 * [MED-C] [IND-3] Compiler optimization must be fixed and documented;
 *          different optimization levels may change stack layout and timing.
 *          IEC 62304 §5.5 requires reproducible builds. */
/* #define OS_OPT_LEVEL_STRING "O2" */

/* Default stack size per task in bytes.
   512 instead of 256: at -O2 the optimizer inlines HAL/library frames
   deeper than at -O0, and 256 left almost no margin (overflow would
   corrupt adjacent TCBs silently).  Shrink back if RAM is tight — the
   per-task peak-stack report in the test summary shows real usage.
 * [MED-B] [MED-C] [IND-1] [IND-2] [IND-3] Stack sizes must be verified
 *          by static analysis or runtime measurement.  IEC 62304 §5.4.3
 *          requires analysis of resource usage.  IEC 61508 Part 3 §7.4.3
 *          requires stack depth analysis for all execution paths. */
#ifndef OS_KERNEL_STACK_SIZE
#define OS_KERNEL_STACK_SIZE  512
#endif


/* ============================================================================
 *  OS_TOOL — Scheduling and Communication Primitives
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* Inter-task signaling (binary/counting events)
 * [MED-B] [MED-C] [IND-2] Required for inter-task communication
 *          in systems with more than one active task.
 *          IEC 62304 §5.4.5 — data integrity between software items. */
#ifndef OS_TOOL_EVENT
#define OS_TOOL_EVENT          1
#endif

/* Mutual exclusion with priority inheritance
 * [MED-B] [MED-C] [IND-2] [IND-3] Required when multiple tasks
 *          share resources (hardware peripherals, shared memory).
 *          Priority inheritance prevents unbounded priority inversion,
 *          which is a safety hazard in real-time systems.
 *          IEC 61508 Part 3 §7.4.13 — avoidance of deadlock and livelock. */
#ifndef OS_TOOL_MUTEX
#define OS_TOOL_MUTEX          1
#endif

/* Bounded FIFO queue (template-based, compile-time capacity)
 * [MED-B] [IND-2] Recommended for typed, bounded data transfer between
 *          tasks.  Compile-time capacity enforces resource bounds.
 *          IEC 62304 §5.4.4 — defined data interfaces between modules. */
#ifndef OS_TOOL_QUEUE
#define OS_TOOL_QUEUE          1
#endif

/* Counting semaphore with max-count
 * [MED-B] [IND-2] Recommended for resource pool management and ISR-to-task
 *          signaling.  Max-count prevents unbounded resource allocation.
 *          IEC 61508 Part 3 §7.4.11 — resource usage must be bounded. */
#ifndef OS_TOOL_SEMAPHORE
#define OS_TOOL_SEMAPHORE      1
#endif

/* Tickless idle: WFI-based sleep for power savings
 * [MED-A] Optional — power management is application-specific.
 *          Disable if deterministic power profile is required. */
#ifndef OS_TOOL_TICKLESS_IDLE
#define OS_TOOL_TICKLESS_IDLE  1
#endif


/* ============================================================================
 *  OS_MONITOR — Runtime Observability
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* Deadline monitoring: detect and act on task deadline misses
 * [MED-B] [MED-C] REQUIRED — IEC 62304 §5.4.3 requires timing analysis;
 *          runtime deadline monitoring provides evidence that timing
 *          requirements are met in production.
 * [IND-2] [IND-3] REQUIRED — IEC 61508 Part 3 Table 5: diagnostic coverage
 *          of software execution timing.  Deadline misses indicate a
 *          potential failure of the safety function. */
#ifndef OS_MONITOR_DEADLINE
#define OS_MONITOR_DEADLINE    0
#endif

/* TCB integrity: magic-number check for memory corruption detection
 * [MED-C] REQUIRED — IEC 62304 §5.4.4: data integrity checks for
 *          safety-related software.  Detects corruption of control data.
 * [IND-3] REQUIRED — IEC 61508 Part 2 Table 3: random hardware fault
 *          metrics apply to data corruption in control structures. */
#ifndef OS_MONITOR_TCB_INTEGRITY
#define OS_MONITOR_TCB_INTEGRITY  0
#endif

/* Error log: circular buffer recording errors with timestamp and task ID
 * [MED-B] [MED-C] REQUIRED — IEC 62304 §5.5.4: logging and tracing
 *          of safety-related events.  Logs must be retrievable for
 *          post-market surveillance (IEC 62304 §8).
 * [IND-1] [IND-2] [IND-3] REQUIRED — IEC 61508 Part 1 §7.4.7: event
 *          recording for fault diagnosis.  Logs support SIL verification.
 *          NOTE: The ring buffer is RAM-only and lost on reset.
 *          Persist logs to non-volatile storage for production use. */
#ifndef OS_MONITOR_ERROR_LOG
#define OS_MONITOR_ERROR_LOG   1
#endif

/* Error log capacity (number of entries)
 * [MED-C] [IND-2] [IND-3] Minimum capacity should be sufficient to
 *          capture all errors during a worst-case operating cycle.
 *          32 entries is a starting point; increase for long cycle times. */
#ifndef OS_MONITOR_ERROR_LOG_SIZE
#define OS_MONITOR_ERROR_LOG_SIZE  32
#endif


/* ============================================================================
 *  OS_SAFETY — Fault Detection and Hardware Protection
 * -------------------------------------------------------------------------- *
 *  1 = enabled, 0 = disabled.  Each feature compiles independently.
 * ============================================================================ */

/* RAM test: background March C- algorithm for SRAM integrity
 * [MED-C] REQUIRED — IEC 62304 §5.4.4: memory integrity verification
 *          for safety-related data.  Single-bit faults in SRAM can
 *          corrupt control variables without detection.
 * [IND-3] [IND-4] REQUIRED — IEC 61508 Part 2 Table 2: diagnostic
 *          coverage for random hardware faults in RAM.  March C-
 *          provides stuck-at fault detection.
 * NOTE: This test is non-destructive but may produce false positives
 *       during active DMA transfers.  Call from idle task only. */
#ifndef OS_SAFETY_RAM_TEST
#define OS_SAFETY_RAM_TEST     0
#endif

/* MPU: hardware memory protection per task (Cortex-M3/M4/M7)
 * [MED-C] REQUIRED — IEC 62304 §5.4.4: spatial isolation between
 *          safety-related and non-safety software items.
 *          Prevents a faulty task from corrupting another task's memory.
 * [IND-3] REQUIRED — IEC 61508 Part 2 Table 2: spatial partitioning
 *          is recommended for SIL 3 to contain faults within modules.
 * NOTE: Requires ARM Cortex-M3+ with MPU.  Cortex-M0/L0 do not
 *       have MPU hardware; set to 0 on those targets. */
#ifndef OS_SAFETY_MPU
#define OS_SAFETY_MPU          0
#endif

/* Hardware watchdog feed/check integration
 * [MED-C] REQUIRED — IEC 62304 §5.4.7: fault tolerance requires
 *          a mechanism to recover from unrecoverable SW faults.
 *          The HW watchdog resets the MCU if the SW hangs.
 * [IND-2] [IND-3] REQUIRED — IEC 61508 Part 2 Table 2: process
 *          interruption / replacement (watchdog as external monitor).
 *          IEC 61508 Part 3 §7.4.14: external monitoring device.
 * NOTE: Configure IWDG timeout via CubeMX.  The feed task must run
 *       at ≤ 50% of the IWDG timeout to prevent spurious resets. */
#ifndef OS_SAFETY_HW_WATCHDOG
#define OS_SAFETY_HW_WATCHDOG  0
#endif

/* CRC check: ROM integrity verification via CRC peripheral
 * [MED-C] REQUIRED — IEC 62304 §5.4.4: code integrity verification.
 *          Detects flash bit-flips from radiation or electrical stress.
 * [IND-3] [IND-4] REQUIRED — IEC 61508 Part 2 Table 2: diagnostic
 *          coverage for program memory.  Periodic CRC re-verification
 *          detects latent faults in executable code.
 * NOTE: Do not write to flash while CRC check is in progress;
 *       this causes false positives. */
#ifndef OS_SAFETY_CRC_CHECK
#define OS_SAFETY_CRC_CHECK    0
#endif

/* Software watchdog: detect stuck tasks and recover
 * [MED-B] [MED-C] REQUIRED — IEC 62304 §5.4.6: error detection
 *          for safety-related tasks.  A stuck task is a failure
 *          mode that must be detected within the safety time.
 * [IND-1] [IND-2] REQUIRED — IEC 61508 Part 3 Table 5: SW fault
 *          detection for diagnostic coverage calculation.
 * NOTE: The timeout must be set to ≤ the process safety time.
 *       Tasks that legitimately run long must yield periodically. */
#ifndef OS_SAFETY_SOFT_WATCHDOG
#define OS_SAFETY_SOFT_WATCHDOG  0
#endif

/* Action on deadline miss: 0 = log only, 1 = reset task, 2 = disable task
 * [MED-B] [MED-C] REQUIRED ≥ 1 — IEC 62304 §5.4.7: fault reaction.
 *          Logging alone (0) is insufficient for Class B/C; the system
 *          must take corrective action (reset or disable).
 * [IND-2] [IND-3] REQUIRED ≥ 1 — IEC 61508 Part 1 §7.4.8:
 *          safety function must respond to detected faults.
 *          Option 1 (reset) enables recovery; option 2 (disable)
 *          enters a safe state. */
#ifndef OS_SAFETY_DEADLINE_ACTION
#define OS_SAFETY_DEADLINE_ACTION  1
#endif

/* Max task recovery attempts before permanent disable
 * [MED-C] [IND-3] Recommended ≥ 1 — IEC 62304 §5.4.7 and
 *          IEC 61508 Part 1 §7.4.8: after repeated failures,
 *          the system should enter a safe state (task disabled)
 *          rather than continue attempting recovery indefinitely.
 *          Set to 0 to disable recovery (immediate safe state). */
#ifndef OS_SAFETY_TASK_MAX_RECOVERY
#define OS_SAFETY_TASK_MAX_RECOVERY  3
#endif

/* Software watchdog timeout in milliseconds
 * [MED-B] [MED-C] REQUIRED — Must be ≤ the application's safety time.
 *          IEC 62304 §5.4.6: error detection time must be less than
 *          the time to reach a hazardous state.
 * [IND-1] [IND-2] REQUIRED — Must be ≤ the process safety time.
 *          IEC 61508 Part 1 §7.4.4: diagnostic test interval.
 *          Common values: 500 ms (motor control), 3000 ms (slow process).
 *          Reduce for faster-responding safety functions. */
#ifndef OS_SAFETY_SOFT_WDG_TIMEOUT_MS
#define OS_SAFETY_SOFT_WDG_TIMEOUT_MS  3000UL
#endif

/* Max duration (us) allowed inside OS_SAFE before reporting error
 * [MED-C] [IND-3] Recommended — Long critical sections increase
 *          interrupt latency, which can cause deadline misses.
 *          IEC 61508 Part 3 §7.4.13: worst-case interrupt latency
 *          must be bounded and documented.
 *          Set to 0 to disable the check. */
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


/* ============================================================================
 *  Safety Standard Enforcement
 * -------------------------------------------------------------------------- *
 *  Define OS_TARGET_MEDICAL to enforce IEC 62304 (Medical SW Lifecycle)
 *  Define OS_TARGET_INDUSTRIAL to enforce IEC 61508 (Functional Safety)
 *  These can be combined — the stricter requirement applies.
 *
 *  Usage:
 *      -DOS_TARGET_MEDICAL=2        // IEC 62304 Class A (set 1, 2, or 3)
 *      -DOS_TARGET_INDUSTRIAL=3     // IEC 61508 SIL (set 1, 2, 3, or 4)
 *
 *  Override in project preprocessor defines, NOT in this file.
 * ============================================================================ */

/* ── IEC 62304 Medical Software Safety Classes ──
 *   Class A: No injury possible (informational, non-critical)
 *   Class B: Non-serious injury possible
 *   Class C: Death or serious injury possible
 *>
 * ── IEC 61508 Industrial Safety Integrity Levels ──
 *   SIL 1: Low risk — basic fault detection
 *   SIL 2: Medium risk — robust fault detection + reaction
 *   SIL 3: High risk — high-integrity fault tolerance
 *   SIL 4: Very high risk — typically hardware, not SW
 */

#ifndef OS_TARGET_MEDICAL
#define OS_TARGET_MEDICAL  0   /* 0 = not targeting medical */
#endif

#ifndef OS_TARGET_INDUSTRIAL
#define OS_TARGET_INDUSTRIAL  0  /* 0 = not targeting industrial */
#endif

/* ── Validate OS_TARGET values ── */
#if (OS_TARGET_MEDICAL < 0) || (OS_TARGET_MEDICAL > 3)
#error "OS_TARGET_MEDICAL must be 0 (disabled), 1 (Class A), 2 (Class B), or 3 (Class C)"
#endif

#if (OS_TARGET_INDUSTRIAL < 0) || (OS_TARGET_INDUSTRIAL > 4)
#error "OS_TARGET_INDUSTRIAL must be 0 (disabled), 1 (SIL 1), 2 (SIL 2), 3 (SIL 3), or 4 (SIL 4)"
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  IEC 62304 Enforcement (Medical)
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Class B and above: fault detection required ── */
#if (OS_TARGET_MEDICAL >= 2)

#if !OS_MONITOR_DEADLINE
#error "[IEC 62304 Class B/C] OS_MONITOR_DEADLINE must be enabled — deadline monitoring is required for fault detection"
#endif

#if !OS_MONITOR_ERROR_LOG
#error "[IEC 62304 Class B/C] OS_MONITOR_ERROR_LOG must be enabled — error logging is required for traceability"
#endif

#if !OS_SAFETY_SOFT_WATCHDOG
#error "[IEC 62304 Class B/C] OS_SAFETY_SOFT_WATCHDOG must be enabled — task fault detection is required"
#endif

#if (OS_SAFETY_DEADLINE_ACTION < 1)
#error "[IEC 62304 Class B/C] OS_SAFETY_DEADLINE_ACTION must be ≥ 1 — logging alone is insufficient, corrective action required"
#endif

#endif /* OS_TARGET_MEDICAL >= 2 */

/* ── Class C: fault detection + tolerance + memory protection required ── */
#if (OS_TARGET_MEDICAL >= 3)

#if !OS_SAFETY_HW_WATCHDOG
#error "[IEC 62304 Class C] OS_SAFETY_HW_WATCHDOG must be enabled — HW watchdog is required for fault tolerance"
#endif

#if !OS_MONITOR_TCB_INTEGRITY
#error "[IEC 62304 Class C] OS_MONITOR_TCB_INTEGRITY must be enabled — control data integrity checks required"
#endif

#if !OS_SAFETY_MPU
#error "[IEC 62304 Class C] OS_SAFETY_MPU must be enabled — spatial isolation required for safety partitioning"
#endif

#if !OS_SAFETY_CRC_CHECK
#error "[IEC 62304 Class C] OS_SAFETY_CRC_CHECK must be enabled — code integrity verification required"
#endif

#if !OS_SAFETY_RAM_TEST
#error "[IEC 62304 Class C] OS_SAFETY_RAM_TEST must be enabled — RAM integrity verification required"
#endif

#endif /* OS_TARGET_MEDICAL >= 3 */


/* ═══════════════════════════════════════════════════════════════════
 *  IEC 61508 Enforcement (Industrial)
 * ═══════════════════════════════════════════════════════════════════ */

/* ── SIL 1+: basic fault detection ── */
#if (OS_TARGET_INDUSTRIAL >= 1)

#if !OS_SAFETY_SOFT_WATCHDOG
#error "[IEC 61508 SIL 1+] OS_SAFETY_SOFT_WATCHDOG must be enabled — SW fault detection required"
#endif

#if !OS_MONITOR_ERROR_LOG
#error "[IEC 61508 SIL 1+] OS_MONITOR_ERROR_LOG must be enabled — event recording required for diagnostics"
#endif

#endif /* OS_TARGET_INDUSTRIAL >= 1 */

/* ── SIL 2+: robust fault detection + reaction ── */
#if (OS_TARGET_INDUSTRIAL >= 2)

#if !OS_MONITOR_DEADLINE
#error "[IEC 61508 SIL 2+] OS_MONITOR_DEADLINE must be enabled — timing diagnostics required"
#endif

#if !OS_TOOL_MUTEX
#error "[IEC 61508 SIL 2+] OS_TOOL_MUTEX must be enabled — mutual exclusion required for shared resources"
#endif

#if !OS_SAFETY_HW_WATCHDOG
#error "[IEC 61508 SIL 2+] OS_SAFETY_HW_WATCHDOG must be enabled — external monitoring device required"
#endif

#if (OS_SAFETY_DEADLINE_ACTION < 1)
#error "[IEC 61508 SIL 2+] OS_SAFETY_DEADLINE_ACTION must be ≥ 1 — corrective action on fault required"
#endif

#endif /* OS_TARGET_INDUSTRIAL >= 2 */

/* ── SIL 3+: high-integrity fault tolerance ── */
#if (OS_TARGET_INDUSTRIAL >= 3)

#if !OS_SAFETY_MPU
#error "[IEC 61508 SIL 3+] OS_SAFETY_MPU must be enabled — spatial partitioning required for fault containment"
#endif

#if !OS_SAFETY_RAM_TEST
#error "[IEC 61508 SIL 3+] OS_SAFETY_RAM_TEST must be enabled — RAM diagnostic coverage required"
#endif

#if !OS_SAFETY_CRC_CHECK
#error "[IEC 61508 SIL 3+] OS_SAFETY_CRC_CHECK must be enabled — program memory verification required"
#endif

#if !OS_MONITOR_TCB_INTEGRITY
#error "[IEC 61508 SIL 3+] OS_MONITOR_TCB_INTEGRITY must be enabled — control data integrity required"
#endif

#endif /* OS_TARGET_INDUSTRIAL >= 3 */
