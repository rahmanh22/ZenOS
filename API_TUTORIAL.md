# ZenOS RTOS — Complete API Tutorial & Cookbook

**Version:** 1.0.0 | **Platform:** ARM Cortex-M (STM32) | **Language:** C++11 / C

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Task Management](#2-task-management)
3. [Timing & Delays](#3-timing--delays)
4. [Critical Sections (OS_SAFE)](#4-critical-sections-os_safe)
5. [Events (OS_EVENT)](#5-events-os_event)
6. [Mutexes (OS_MUTEX)](#6-mutexes-os_mutex)
7. [Queues (OS_QUEUE)](#7-queues-os_queue)
8. [Semaphores (OS_SEMAPHORE)](#8-semaphores-os_semaphore)
9. [Periodic Tasks — The ZenOS Way (No Software Timers)](#9-periodic-tasks--the-zenos-way-no-software-timers)
10. [Error Handling & Monitoring](#10-error-handling--monitoring)
11. [Safety Features](#11-safety-features)
12. [Advanced Tips & Tricks](#12-advanced-tips--tricks)
13. [Common Patterns Cookbook](#13-common-patterns-cookbook)

---

## 1. Quick Start

### 1.1 Include & Initialize

```cpp
#include "ZenOS.hpp"

int main() {
    HAL_Init();
    SystemClock_Config();
    // ... initialize peripherals (UART, GPIO, etc.) ...

    os_init();  // Initialize RTOS internals

    // Create tasks BEFORE os_start()
    os_task_create(task_main, 5);       // priority 5, default stack
    os_task_create(task_sensor, 3, 100); // priority 3, 100ms periodic
    os_task_create_st(task_ui, 8, 0, 1024); // priority 8, custom 1KB stack

    os_start();  // Never returns — scheduler takes over
}
```

### 1.2 Minimal Task

```cpp
void task_blink(void) {
    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        os_delay_ms(500);  // Yield for 500ms
    }
}
```

> **💡 Tip:** Every task MUST have `while(1)` and MUST yield periodically (via `os_delay_ms`, `os_yield`, or blocking on IPC). A task that never yields will be detected by the software watchdog and reset.

### 1.3 The Boot Sequence

```
os_init()  →  create tasks  →  os_start()  →  scheduler runs forever
    ↑                                                        ↓
    └──────── os_start() never returns ──────────────────────┘
```

**Critical rule:** All tasks must be created **before** `os_start()`. Task creation after `os_start()` returns -1 and reports `TASK_AFTER_START`.

---

## 2. Task Management

### 2.1 Creating Tasks

```cpp
// Default stack size (OS_KERNEL_STACK_SIZE = 512 bytes)
os_task_create(task_function, priority);         // priority 1-255
os_task_create(task_function, priority, period_ms); // + periodic

// Custom stack size (in bytes)
os_task_create_st(task_function, priority, period_ms, stack_bytes);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `entry` | `void(*)(void)` | Task function pointer (used as unique ID) |
| `priority` | `uint8_t` | 1 = lowest, 255 = highest. 0 = idle task (reserved) |
| `period_ms` | `uint32_t` | 0 = aperiodic (run whenever scheduled), >0 = periodic |
| `stack_bytes` | `uint32_t` | Stack size in bytes (default: 512) |

### 2.2 Starting & Stopping Tasks

```cpp
os_task_start(task_function);    // Activate a stopped task
os_task_stop(task_function);     // Deactivate (does not destroy)

// Check state
bool running = os_task_isActive(task_function);
uint8_t state = os_task_get_state(task_function); // TaskState enum
```

> **💡 Tip:** Stopping a task does NOT free its memory — it's statically allocated. You can start/stop the same task as many times as you need. This is the primary mechanism for runtime task control.

### 2.3 Task Priority

```cpp
uint8_t prio = os_get_task_priority(task_function);
// Returns the ACTIVE priority (may be temporarily boosted by IPC ceiling)
```

**Priority guidelines:**
- 1: Lowest application priority
- 2–4: Background tasks (sensors, logging)
- 5–10: Normal tasks (UI, communication)
- 11–20: High-priority tasks (safety monitoring)
- 21+: ISR-like tasks (extremely time-critical)
- 255: Reserved (do not use)

### 2.4 Periodic Tasks

```cpp
// Task runs every 100ms automatically
void task_sensor(void) {
    while (1) {
        uint16_t adc_val = read_adc();
        // ... process data ...
        os_delay_ms(10); // Yield — next run scheduled at next period
    }
}

// Create with 100ms period
os_task_create(task_sensor, 3, 100);
```

> **💡 Tip:** For periodic tasks, use `os_delay_ms()` at the end of each iteration. The scheduler will automatically wake the task at the next period boundary. See [Section 9](#9-periodic-tasks--the-zenos-way-no-software-timers) for a deep dive.

### 2.5 Stack Sizing Guide

| Task Type | Recommended Stack |
|-----------|------------------|
| Simple LED blink | 128–256 bytes |
| Sensor reading (HAL) | 256–512 bytes |
| UART communication | 512–1024 bytes |
| SPI/I2C + protocol stack | 512–1024 bytes |
| printf / float formatting | 1024–2048 bytes |
| Complex application logic | 1024–2048 bytes |

> **💡 Tip:** Use `os_get_stack_usage(task)` during development to measure actual peak usage, then set stack to ≥130% of peak.

---

## 3. Timing & Delays

### 3.1 Millisecond Delay

```cpp
os_delay_ms(100);  // Sleep for ~100ms, yield CPU to other tasks
os_delay_ms(0);    // Immediate yield (gives other tasks a chance to run)
```

### 3.2 Microsecond Delay (Busy-Wait)

```cpp
os_delay_us(10);    // Busy-wait ~10µs (blocks CPU — no other task runs!)
os_delay_us(1000);  // ~1ms busy-wait
```

> **⚠️ Warning:** `os_delay_us()` uses the DWT cycle counter for busy-waiting. It does NOT yield the CPU. Use only for very short delays (< 1ms). For longer delays, use `os_delay_ms()`.

### 3.3 Getting Current Time

```cpp
uint32_t ticks = os_get_tick();  // Raw tick count
uint32_t ms    = os_get_ms();    // Milliseconds since boot
uint32_t us    = os_get_us();    // Microseconds since boot (DWT cycle counter)
```

### 3.4 Timing Patterns

**Elapsed time measurement:**
```cpp
uint32_t t0 = os_get_ms();
// ... do work ...
uint32_t elapsed = os_get_ms() - t0;  // Works even if ms wraps (~49 days)
```

**Non-blocking periodic check:**
```cpp
static uint32_t last_check = 0;
if (os_get_ms() - last_check >= 100) {
    last_check = os_get_ms();
    // ... do periodic work ...
}
```

> **💡 Tip:** Always use the subtraction pattern `os_get_ms() - last >= interval` instead of absolute comparisons. This handles the 32-bit millisecond counter wrapping correctly.

---

## 4. Critical Sections (OS_SAFE)

### 4.1 Basic Usage

```cpp
volatile uint32_t shared_counter = 0;

// In task A:
OS_SAFE {
    shared_counter++;  // Interrupts are disabled here
}
// Interrupts are re-enabled here
```

`OS_SAFE` disables all interrupts for the duration of the block, then re-enables them. This is the **only safe way** to protect shared variables between tasks and ISRs.

### 4.2 What OS_SAFE Actually Does

```
┌─ OS_SAFE { ─────────────────────────┐
│  1. Disable interrupts (CPSID I)     │
│  2. Enter guarded block              │
│  3. Enable interrupts (CPSIE I)      │
└──────────────────────────────────────┘
```

The implementation uses RAII (C++ `_OsSafeGuard` class) — interrupts are always re-enabled, even if an exception occurs.

### 4.3 Nesting

```cpp
OS_SAFE {
    shared_counter++;
    OS_SAFE {
        // Nested — depth is tracked, interrupt state is preserved
        another_var++;
    }
    // Still in outer OS_SAFE — interrupts still disabled
}
// Interrupts re-enabled here
```

### 4.4 Duration Monitoring

If `OS_SAFETY_MAX_CRITICAL_US` is configured (default 1000µs), a critical section that exceeds the limit triggers a `SAFE_TOO_LONG` error.

**Keep critical sections short!**
```cpp
// ✅ GOOD — short critical section
OS_SAFE { shared_counter++; }

// ❌ BAD — long critical section blocks all interrupts
OS_SAFE {
    HAL_UART_Transmit(&huart1, data, len, 1000);  // Blocks for ~10ms!
}
```

### 4.5 OS_SAFE vs OS_LOCK

| Feature | `OS_SAFE` | `OS_LOCK` |
|---------|----------|----------|
| Disables interrupts | ✅ Yes | ❌ No |
| Allows blocking | ❌ No (will error) | ✅ Yes |
| Protects from ISRs | ✅ Yes | ❌ No |
| Protects from other tasks | ✅ Yes | ✅ Yes |
| Use case | Share data with ISRs | Protect shared resources between tasks |

> **💡 Tip:** Use `OS_SAFE` only when you need to share data with interrupt handlers. For task-to-task synchronization, prefer `OS_LOCK` (mutex) — it doesn't block interrupts.

---

## 5. Events (OS_EVENT)

### 5.1 Basic Signaling

```cpp
OS_EVENT sensor_ready;

// Task A (waiter):
if (sensor_ready.wait(1000)) {  // Wait up to 1000ms
    // Event was signaled
    process_sensor_data();
} else {
    // Timeout — event not signaled in time
    handle_timeout();
}

// Task B (signaler):
read_sensor();
sensor_ready.signal();  // Wake up the waiter
```

### 5.2 ISR-to-Task Signaling

```cpp
OS_KEY_PRESS;

// In EXTI ISR:
void EXTI0_IRQHandler(void) {
    OS_KEY_PRESS.signal_from_isr();  // ISR-safe version
    __HAL_GPIO_EXTI_CLEAR_PIN(...);
}

// In task:
void task_handle_input(void) {
    while (1) {
        if (OS_KEY_PRESS.wait(5000)) {
            // Key was pressed
        }
    }
}
```

> **💡 Tip:** Always use `signal_from_isr()` in interrupt handlers. Calling `signal()` from an ISR is undefined behavior.

### 5.3 Signal Accumulation

```cpp
OS_EVENT evt;

evt.signal();  // count = 1
evt.signal();  // count = 2
evt.signal();  // count = 3

evt.wait();    // count = 2 (returns 1)
evt.wait();    // count = 1 (returns 1)
evt.wait();    // count = 0 (returns 1)
evt.wait();    // returns 0 (timeout — count was already 0)
```

### 5.4 Waiting for Multiple Events

```cpp
OS_EVENT evt_a, evt_b;

// Wait for EITHER event (whichever comes first)
uint32_t mask = evt_a | evt_b;  // Works for IDs 0-31 only
// Use os_event_signal_from_isr(mask) for ISR signaling

// Individual wait with short timeout as polling
while (1) {
    if (evt_a.wait(10) == 1) {
        handle_a();
    }
    if (evt_b.wait(10) == 1) {
        handle_b();
    }
}
```

### 5.5 Event ID Limitations

- Events are auto-assigned IDs starting from 0
- IDs 0–31: Full support (mask-based signaling, `|` operator)
- IDs ≥ 32: Supported but must be signaled individually (no mask)
- Practical limit: hundreds of events per system

---

## 6. Mutexes (OS_MUTEX)

### 6.1 Basic Usage

```cpp
OS_MUTEX uart_mtx;  // Global mutex

// Task A:
OS_LOCK(uart_mtx) {
    HAL_UART_Transmit(&huart1, data_a, len, 100);
}  // Automatically unlocked when block ends

// Task B:
OS_LOCK(uart_mtx) {
    HAL_UART_Transmit(&huart1, data_b, len, 100);
}
```

`OS_LOCK` is an RAII lock guard — it acquires the mutex on entry and releases it on exit (including early returns and exceptions).

### 6.2 Manual Lock/Unlock

```cpp
OS_MUTEX mtx;

if (mtx.lock(1000)) {  // Wait up to 1000ms
    // Mutex acquired
    do_work();
    mtx.unlock();
} else {
    // Timeout
}
```

### 6.3 Recursive Locking

```cpp
OS_MUTEX mtx;

OS_LOCK(mtx) {
    OS_LOCK(mtx) {  // Recursive — same task can lock again
        OS_LOCK(mtx) {  // Triple nesting works too
            // Still protected
        }
    }
}  // All three levels unlocked
```

### 6.4 IPC Ceiling (Priority Inheritance)

The IPC ceiling prevents priority inversion — a critical safety issue in real-time systems.

```cpp
// Task priorities: sensor=2, controller=5, safety=8

// Mutex with ceiling 8 (highest user's priority)
OS_MUTEX shared_data_mtx(8);

// sensor (prio 2) locks the mutex → temporarily boosted to prio 8
// safety (prio 8) waits for mutex → sensor boosted, runs immediately
// controller (prio 5) cannot preempt → no inversion!
```

**How it works:**
1. When a task locks a mutex, its priority is boosted to the ceiling
2. The ceiling should be set to the highest priority of any task that acquires the mutex
3. On unlock, priority returns to the original (base) value

```cpp
// Correct ceiling configuration
OS_MUTEX sensor_mtx(8);  // Ceiling = 8 (safety task's priority)
// sensor(2), controller(5), safety(8) all use this mutex
// When sensor holds it, sensor runs at priority 8
// safety never has to wait for controller — inversion prevented
```

> **💡 Tip:** Set the ceiling to the priority of the **highest-priority task** that ever locks the mutex. An incorrect ceiling can cause priority inversion or priority conflicts.

### 6.5 Mutex Safety Rules

| Rule | Consequence of Violation |
|------|------------------------|
| Always unlock what you lock | Mutex stays locked forever, other tasks deadlock |
| Lock/unlock in same scope | Risk of forgetting to unlock |
| Never lock from ISR | Will block forever — ISRs cannot yield |
| Never use `OS_SAFE` + `OS_LOCK` together | `OS_SAFE` disables interrupts, `OS_LOCK` needs them for yielding |
| Match lock count to unlock count | Double-unlock is tolerated but increments error counter |

> **💡 Tip:** Always prefer `OS_LOCK(mtx) { ... }` over manual `lock()`/`unlock()`. The RAII guard ensures the mutex is always released, even if you return early or an exception occurs.

---

## 7. Queues (OS_QUEUE)

### 7.1 Basic Usage

```cpp
OS_QUEUE<int, 8> sensor_queue;  // Capacity: 8 items of type int

// Producer task:
sensor_queue.put(42);          // Block if full
sensor_queue.put(100, 100);    // Wait max 100ms, return false if still full

// Consumer task:
int value;
if (sensor_queue.get(value, 1000)) {  // Wait up to 1000ms
    process(value);
}
```

### 7.2 ISR-Safe Queue Operations

```cpp
OS_QUEUE<uint16_t, 32> adc_queue;

// In ADC ISR:
void ADC_IRQHandler(void) {
    uint16_t val = HAL_ADC_GetValue(&hadc1);
    adc_queue.put_from_isr(val);  // Non-blocking, returns false if full
}

// In task:
void task_process_adc(void) {
    while (1) {
        uint16_t val;
        if (adc_queue.get(val, 100)) {
            // Process ADC value
        }
    }
}
```

> **💡 Tip:** `put_from_isr()` is non-blocking — it returns `false` immediately if the queue is full. This is safe to call from any ISR without disabling interrupts.

### 7.3 Queue State Queries

```cpp
OS_QUEUE<int, 16> q;

q.put(1); q.put(2); q.put(3);

q.get_count();     // 3
q.get_capacity();  // 16
q.is_full();       // false
q.is_empty();      // false

q.reset();         // Clear all items
q.is_empty();      // true
q.get_count();     // 0
```

### 7.4 Typed Queues

```cpp
// Queue of structs
struct SensorData {
    uint16_t temperature;
    uint16_t humidity;
    uint32_t timestamp;
};

OS_QUEUE<SensorData, 16> sensor_data_queue;

// Queue of pointers (for variable-size data)
OS_QUEUE<void*, 8> msg_queue;
```

> **💡 Tip:** `OS_QUEUE` is a template — capacity is fixed at compile time. Choose capacity based on worst-case burst scenarios. Too small = producer blocked; too large = wasted RAM.

---

## 8. Semaphores (OS_SEMAPHORE)

### 8.1 Binary Semaphore (Resource Lock)

```cpp
OS_SEMAPHORE spi_sem(1);  // Initial count = 1 (one resource)

// Task A:
spi_sem.wait();   // Acquire (count → 0)
SPI_Transmit(data);
spi_sem.signal(); // Release (count → 1)

// Task B:
spi_sem.wait();   // Waits if A holds it
SPI_Transmit(data2);
spi_sem.signal();
```

### 8.2 Counting Semaphore (Resource Pool)

```cpp
OS_SEMAPHORE pool_sem(3);  // 3 resources available

pool_sem.wait();  // count = 2
pool_sem.wait();  // count = 1
pool_sem.wait();  // count = 0
pool_sem.wait(100); // Timeout — no resources available

pool_sem.signal();  // count = 1
pool_sem.signal();  // count = 2
```

### 8.3 Bounded Semaphore

```cpp
OS_SEMAPHORE bounded_sem(0, 5);  // Initial 0, max 5

bounded_sem.signal();  // count = 1
bounded_sem.signal();  // count = 2
bounded_sem.signal();  // count = 3
bounded_sem.signal();  // count = 4
bounded_sem.signal();  // count = 5
bounded_sem.signal();  // Returns false — max reached!
```

### 8.4 ISR-Safe Signaling

```cpp
OS_SEMAPHORE data_ready(0);

// In ISR:
void TIM2_IRQHandler(void) {
    data_ready.signal_from_isr();  // ISR-safe
}

// In task:
void task_process(void) {
    while (1) {
        if (data_ready.wait(1000)) {
            process_data();
        }
    }
}
```

---

## 9. Periodic Tasks — The ZenOS Way (No Software Timers)

### 9.1 Why No Software Timers?

ZenOS does **not** provide a software timer API. Instead, it uses **periodic tasks** — a superior approach that offers:

| Feature | Periodic Task | Software Timer |
|---------|--------------|----------------|
| **Memory** | Own stack (dedicated) | Shared callback stack |
| **Context** | Full task context | Callback context (limited) |
| **Blocking** | ✅ Can block on IPC, mutexes | ❌ Must return quickly |
| **Safety** | ✅ Stack overflow detected per task | ❌ Shared stack — overflow harder to detect |
| **Priority** | ✅ Independent per task | ❌ Typically runs at timer task priority |
| **MPU protection** | ✅ Each task gets its own MPU region | ❌ All callbacks share one region |
| **Error isolation** | ✅ One task fault doesn't affect others | ❌ Timer callback fault can crash system |
| **Cancellation** | ✅ `os_task_stop()` | ❌ Must track and cancel callback |

### 9.2 Creating a Periodic Task

```cpp
// Method 1: Create with period (automatic scheduling)
void task_read_sensor(void) {
    while (1) {
        uint16_t val = read_adc();
        send_to_queue(val);
        os_delay_ms(10);  // Yield — next run at period boundary
    }
}

os_task_create(task_read_sensor, 3, 100);  // Every 100ms

// Method 2: Manual periodic (more control)
void task_control_loop(void) {
    while (1) {
        uint32_t t0 = os_get_ms();
        
        read_sensors();
        compute_output();
        apply_actuator();
        
        // Precise 10ms period with drift compensation
        uint32_t elapsed = os_get_ms() - t0;
        if (elapsed < 10) {
            os_delay_ms(10 - elapsed);
        }
    }
}

os_task_create(task_control_loop, 10);  // Aperiodic — we handle timing
```

### 9.3 Comparing Approaches

**❌ Software Timer approach (not available in ZenOS):**
```cpp
// Hypothetical — this does NOT exist in ZenOS
void timer_callback(void* arg) {
    uint16_t val = read_adc();  // Must return quickly!
    send_to_queue(val);         // Can't block on mutexes!
}
timer_create(100, timer_callback);  // One callback for all uses
```

**✅ ZenOS Periodic Task approach:**
```cpp
// Each task has its own stack, priority, and error isolation
void task_read_sensor(void) {
    while (1) {
        OS_LOCK(spi_mtx) {                    // Can use mutexes!
            uint16_t val = SPI_Read();         // Can use HAL freely
            OS_QUEUE_LOCK(sensor_queue) {      // Can block on queue
                sensor_queue.put(val);
            }
        }
        os_delay_ms(10);
    }
}
os_task_create(task_read_sensor, 3, 100);
```

### 9.4 Timing Precision

For precise periodic execution, compensate for execution time:

```cpp
void task_pid_controller(void) {
    while (1) {
        uint32_t t0 = os_get_us();
        
        // === Control work ===
        float error = setpoint - read_encoder();
        integral += error * dt;
        float output = Kp * error + Ki * integral + Kd * (error - prev_error);
        set_actuator(output);
        prev_error = error;
        // ===================
        
        // Compensate for execution time
        uint32_t elapsed_us = os_get_us() - t0;
        uint32_t period_us = 1000;  // 1ms period
        if (elapsed_us < period_us) {
            os_delay_us(period_us - elapsed_us);  // Sub-tick precision
        }
        // If elapsed_us >= period_us, we're already late — run next iteration immediately
    }
}

os_task_create(task_pid_controller, 20);  // High priority, aperiodic
```

### 9.5 Task Communication Patterns

**Producer-Consumer with Queue:**
```cpp
OS_QUEUE<SensorData, 16> data_queue;

void task_sensor(void) {
    while (1) {
        SensorData s = read_all_sensors();
        data_queue.put(s);  // Blocks if queue full (backpressure)
        os_delay_ms(50);
    }
}

void task_logger(void) {
    while (1) {
        SensorData s;
        if (data_queue.get(s, 1000)) {
            log_to_flash(s);
        }
    }
}
```

**ISR → Task with Semaphore:**
```cpp
OS_SEMAPHORE data_sem(0);

void EXTI1_IRQHandler(void) {
    data_sem.signal_from_isr();
}

void task_handle_event(void) {
    while (1) {
        if (data_sem.wait(5000)) {
            handle_exti_event();
        } else {
            log_timeout();
        }
    }
}
```

---

## 10. Error Handling & Monitoring

### 10.1 Error Reporting

```cpp
// Report a custom error
os_log_error(OSError::SENSOR_TIMEOUT, (uint8_t)ErrorSeverity::WARNING);

// Check error counts
uint32_t total   = os_get_error_count();
uint32_t unexpected = os_get_unexpected_error_count();
uint32_t expected   = os_get_expected_error_count();

OSError last = os_get_last_error();
```

### 10.2 Suppressing Expected Errors

During testing, you may deliberately trigger errors. Use `OS_ERROR_EXPECTED` to exclude them from the unexpected count:

```cpp
// In test code:
OS_ERROR_EXPECTED {
    os_event_signal(-1);  // Deliberately invalid — expected error
}
// os_get_unexpected_error_count() does NOT increase
```

### 10.3 Error Log

```cpp
#if OS_MONITOR_ERROR_LOG
    // Log an error
    os_log_error(OSError::STACK_OVERFLOW, (uint8_t)ErrorSeverity::CRITICAL);

    // Read log entries (most recent first)
    uint32_t count = os_get_error_log_count();
    for (uint32_t i = 0; i < count; i++) {
        OSErrorEntry entry = os_get_error_log_entry(i);
        // entry.timestamp_tick — when it happened
        // entry.code          — which error
        // entry.task_id       — which task
        // entry.severity      — INFO, WARNING, CRITICAL
    }
#endif
```

### 10.4 Stack Usage Monitoring

```cpp
#if OS_MONITOR_ENABLED
    // Per-task stack usage
    uint32_t bytes = os_get_stack_usage(task_function);

    // Full report
    uint8_t count = os_get_stack_report_count();
    for (uint8_t i = 0; i < count; i++) {
        os_stack_report_entry_t rep;
        os_get_stack_report(i, &rep);
        // rep.name       — task name
        // rep.size_bytes — total stack size
        // rep.peak_bytes — peak usage since last start/reset
    }

    // CPU usage
    uint8_t total_cpu = os_get_cpu_usage_total();
    uint8_t task_cpu  = os_get_task_cpu_usage(task_function);
#endif
```

### 10.5 Deadline Monitoring

```cpp
#if OS_MONITOR_DEADLINE
    // Set deadline for a task (must be called after creation)
    os_task_set_deadline(task_safety, 50);  // 50ms deadline

    // Check misses
    uint32_t misses = os_get_deadline_miss_count(task_safety);
    if (misses > 0) {
        // Handle deadline miss (log, reset, etc.)
    }
#endif
```

---

## 11. Safety Features

### 11.1 Hardware Watchdog

```cpp
#if OS_SAFETY_HW_WATCHDOG
    // In a dedicated task:
    void task_wdg_feeder(void) {
        while (1) {
            os_hw_watchdog_feed();  // Unconditional feed
            os_delay_ms(1000);     // Feed every 1s (IWDG timeout ~6.5s)
        }
    }

    // Or conditional feed (skip if unhealthy):
    os_hw_watchdog_check();  // Only feeds if error_count < 10
#endif
```

### 11.2 RAM Test (Background)

```cpp
#if OS_SAFETY_RAM_TEST
    // Call from idle task or low-priority background task
    void task_background(void) {
        while (1) {
            os_ram_test_step();  // Test one word per call
            
            if (os_ram_test_complete()) {
                if (os_get_ram_test_error_count() > 0) {
                    // SRAM fault detected!
                    os_log_error(OSError::RAM_TEST_FAIL, 2);
                }
                // After completion, call os_ram_test_step() resets and starts over
            }
            
            os_yield();  // Let other tasks run
        }
    }
#endif
```

### 11.3 CRC Check (ROM Integrity)

```cpp
#if OS_SAFETY_CRC_CHECK
    // At boot (before os_start):
    os_crc_init();  // Compute expected CRC over flash

    // In background task:
    void task_crc_check(void) {
        while (1) {
            os_crc_check_step();  // Check 64 words per call
            
            if (os_crc_check_complete()) {
                if (os_get_crc_error_count() > 0) {
                    // ROM corruption detected!
                    os_log_error(OSError::HARDFAULT, 2);
                    // Consider triggering a controlled reset
                }
            }
            
            os_delay_ms(100);  // Run every 100ms
        }
    }
#endif
```

### 11.4 MPU Protection

```cpp
#if OS_SAFETY_MPU
    // Initialize MPU at boot (before os_start):
    os_mpu_init();
    os_mpu_enable();

    // Add per-task memory regions:
    extern TCB task_sensor_tcb;
    os_mpu_add_region(&task_sensor_tcb,
        0x20001000,   // Base address
        4096,          // Size (4KB)
        3              // AP: Full access, XN
    );
#endif
```

---

## 12. Advanced Tips & Tricks

### 12.1 Task Lifecycle Management

```cpp
// Pre-create all tasks, start/stop as needed
os_task_create(task_heater, 5);
os_task_create(task_cooler, 5);
os_task_stop(task_heater);   // Start inactive
os_task_stop(task_cooler);   // Start inactive

// Runtime control
if (temperature > 70) {
    os_task_start(task_cooler);
    os_task_stop(task_heater);
} else if (temperature < 20) {
    os_task_start(task_heater);
    os_task_stop(task_cooler);
}
```

### 12.2 Graceful Degradation

```cpp
void task_main(void) {
    while (1) {
        // Try high-accuracy mode
        if (sensor_available()) {
            read_high_accuracy();
        } else {
            // Fallback to low-accuracy mode
            read_low_accuracy();
        }
        
        os_delay_ms(100);
    }
}
```

### 12.3 Power Management with Tickless Idle

```cpp
// Enable OS_TOOL_TICKLESS_IDLE in config
// The OS will use WFI (Wait For Interrupt) when no tasks are ready
// This puts the CPU into low-power mode automatically

// Design tips for low power:
// 1. Use os_delay_ms() instead of busy-wait loops
// 2. Long delays → deep sleep
// 3. Short delays → partial sleep
// 4. Periodic tasks wake up automatically from WFI
```

### 12.4 Debugging Tips

```cpp
// 1. Print task count at startup
uart_printf("Tasks: %d\n", os_get_task_count());

// 2. Monitor stack usage periodically
uint32_t peak = os_get_stack_usage(task_function);
uart_printf("Stack peak: %d bytes\n", peak);

// 3. Check CPU usage
uint8_t cpu = os_get_task_cpu_usage(task_function);
uart_printf("Task CPU: %d%%\n", cpu);

// 4. Print version
uart_printf("ZenOS %s\n", os_get_version_string());

// 5. Check for unexpected errors
if (os_get_unexpected_error_count() > 0) {
    uart_printf("UNEXPECTED ERRORS: %d\n", os_get_unexpected_error_count());
}
```

### 12.5 Compiler Optimization Tips

```cpp
// Mark performance-critical data as volatile if accessed from ISRs
static volatile uint32_t isr_counter = 0;

// Use OS_SAFE for short shared-variable updates
OS_SAFE { isr_counter++; }

// For long ISR-to-task data transfers, use queues instead
OS_QUEUE<uint16_t, 32> isr_data_queue;
// In ISR: isr_data_queue.put_from_isr(val);
// In task: isr_data_queue.get(val, 100);
```

---

## 13. Common Patterns Cookbook

### 13.1 State Machine in a Task

```cpp
enum State { INIT, RUNNING, ERROR, SLEEP };
static State state = INIT;

void task_state_machine(void) {
    while (1) {
        switch (state) {
            case INIT:
                if (hardware_ready()) state = RUNNING;
                break;
            case RUNNING:
                if (error_detected()) state = ERROR;
                else do_work();
                break;
            case ERROR:
                if (error_cleared()) state = RUNNING;
                else enter_safe_state();
                break;
            case SLEEP:
                os_delay_ms(1000);
                break;
        }
        os_delay_ms(10);  // Always yield
    }
}
```

### 13.2 Debounced Button Handling

```cpp
OS_KEY_EVT;

void EXTI0_IRQHandler(void) {
    OS_KEY_EVT.signal_from_isr();
}

void task_button(void) {
    while (1) {
        if (OS_KEY_EVT.wait(5000)) {
            os_delay_ms(50);  // Debounce delay
            if (read_button()) {
                handle_button_press();
            }
        }
    }
}
```

### 13.3 Software PWM via Periodic Task

```cpp
void task_pwm(void) {
    uint8_t duty = 128;  // 50% duty
    while (1) {
        HAL_GPIO_WritePin(PWM_PORT, PWM_PIN, GPIO_PIN_SET);
        os_delay_us(duty * 10);  // ON time
        
        HAL_GPIO_WritePin(PWM_PORT, PWM_PIN, GPIO_PIN_RESET);
        os_delay_us((255 - duty) * 10);  // OFF time
    }
}

os_task_create(task_pwm, 15);  // High priority for timing
```

### 13.4 Multi-Stage Sensor Pipeline

```cpp
OS_QUEUE<RawData, 8> raw_queue;
OS_QUEUE<ProcessedData, 8> processed_queue;

void task_acquire(void) {
    while (1) {
        RawData raw = read_sensor();
        raw_queue.put(raw);
        os_delay_ms(10);
    }
}

void task_process(void) {
    while (1) {
        RawData raw;
        if (raw_queue.get(raw, 100)) {
            ProcessedData proc = filter_and_calibrate(raw);
            processed_queue.put(proc);
        }
    }
}

void task_output(void) {
    while (1) {
        ProcessedData proc;
        if (processed_queue.get(proc, 100)) {
            display(proc);
        }
    }
}

// Pipeline: acquire(3) → process(5) → output(4)
os_task_create(task_acquire, 3, 10);
os_task_create(task_process, 5);
os_task_create(task_output, 4);
```

### 13.5 Graceful Shutdown

```cpp
static volatile bool shutdown_requested = false;

void task_shutdown_handler(void) {
    while (1) {
        if (shutdown_button.wait(1000)) {
            shutdown_requested = true;
            
            // Stop non-essential tasks
            os_task_stop(task_ui);
            os_task_stop(task_led);
            
            // Save state to flash
            save_critical_data();
            
            // Enter safe state
            enter_safe_mode();
        }
    }
}
```

---

## Appendix: API Quick Reference

### Core Functions (C)

| Function | Description |
|----------|-------------|
| `os_init()` | Initialize RTOS (call before task creation) |
| `os_start()` | Start scheduler (never returns) |
| `os_delay_ms(ms)` | Sleep for `ms` milliseconds |
| `os_delay_us(us)` | Busy-wait for `us` microseconds |
| `os_yield()` | Yield to next ready task |
| `os_get_tick()` | Get raw tick count |
| `os_get_ms()` | Get milliseconds since boot |
| `os_get_us()` | Get microseconds since boot |
| `os_task_start(entry)` | Start a stopped task |
| `os_task_stop(entry)` | Stop a running task |
| `os_task_isActive(entry)` | Check if task is active |
| `os_get_task_count()` | Get number of registered tasks |
| `os_get_task_priority(entry)` | Get task's active priority |

### RAII Classes (C++)

| Class | Macro | Description |
|-------|-------|-------------|
| `_OsSafeGuard` | `OS_SAFE { ... }` | Interrupt-disable guard |
| `_OsLockGuard` | `OS_LOCK(mtx) { ... }` | Mutex lock guard |
| `OS_EVENT` | — | Inter-task event signaling |
| `OS_MUTEX` | — | Mutex with priority inheritance |
| `OS_QUEUE<T,N>` | — | Bounded FIFO queue |
| `OS_SEMAPHORE` | — | Counting semaphore |

---

*Generated for ZenOS RTOS v1.0.0 — See also: SAFETY_MANUAL.md, ZenOS_Config.hpp*
