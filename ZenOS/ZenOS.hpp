#pragma once
/**
 * @file    ZenOS.hpp
 * @brief   ZenOS RTOS — Main Public API
 *
 * Safe to include from both C and C++ files.
 * C++ features (enums, classes, templates) are guarded by __cplusplus.
 *
 * @author  Rahman Heidari <rahman.h22@gmail.com> — Raymon Research Team
 * @version 1.0.0
 */

#include <stdint.h>
#include <stddef.h>
#include "ZenOS_Config.hpp"
#include "ZenOS_Port.hpp"


/* ============================================================================
 *  Derived Constants (auto-calculated from config)
 * ============================================================================ */

/* Any monitoring feature enabled */
#if OS_MONITOR_DEADLINE || OS_MONITOR_TCB_INTEGRITY || OS_MONITOR_ERROR_LOG
#define OS_MONITOR_ENABLED  1
#else
#define OS_MONITOR_ENABLED  0
#endif


/* ============================================================================
 *  Event Pool Size (based on available RAM)
 *  ---------------------------------------------------------------------------
 *  Uses 40% of RAM for event pool.
 *  Each event needs ~32 bytes.
 * ============================================================================ */


/* ============================================================================
 *  Idle & Fault Stacks (fixed sizes)
 * ============================================================================ */

#define OS_IDLE_STACK_WORDS     64
#define OS_IDLE_STACK_SIZE      (OS_IDLE_STACK_WORDS * 4)

#define OS_FAULT_STACK_WORDS    48
#define OS_FAULT_STACK_SIZE     (OS_FAULT_STACK_WORDS * 4)

/* ============================================================================
 *  Tick Rate & Constants
 * ============================================================================ */

#define OS_TICKS_PER_MS   (1000UL / OS_KERNEL_TICK_PERIOD_US)
#define OS_WAIT_FOREVER   0xFFFFFFFFUL


/* ============================================================================
 *  Interrupt Helpers (inline — zero overhead)
 * ============================================================================ */

static inline void os_hw_disable_irq() { __asm volatile("cpsid i" ::: "memory"); }
static inline void os_hw_enable_irq()  { __asm volatile("cpsie i" ::: "memory"); }


/* ============================================================================
 *  C-Accessible Declarations
 *  ---------------------------------------------------------------------------
 *  These are visible from both C and C++ files.
 * ============================================================================ */

/* ============================================================================
 *  Version Constants
 * ============================================================================ */

#define OS_VERSION_MAJOR           1
#define OS_VERSION_MINOR           0
#define OS_VERSION_PATCH           0
#define OS_VERSION_PACKED          ((OS_VERSION_MAJOR << 16) | (OS_VERSION_MINOR << 8) | OS_VERSION_PATCH)

#define OS_STR_(x) #x
#define OS_STR(x)  OS_STR_(x)
#define OS_VERSION_STRING  OS_STR(OS_VERSION_MAJOR) "." OS_STR(OS_VERSION_MINOR) "." OS_STR(OS_VERSION_PATCH)


#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t SystemCoreClock;

void os_init(void);
void os_start(void);
void os_tick(void);

/* Critical section — always visible (needed by OS_QUEUE/SEMAPHORE templates) */
uint32_t os_critical_enter(void);
void     os_critical_exit(uint32_t old);
extern volatile uint32_t os_safe_depth;

/* Delay & Yield */
void os_delay_ms(uint32_t ms);
void os_delay_us(uint32_t us);
void os_yield(void);

/* Task Management */
void os_task_stop(void(*entry)(void));
void os_task_start(void(*entry)(void));
uint16_t os_get_task_count(void);
bool os_task_isActive(void(*entry)(void));
uint8_t os_get_task_priority(void(*entry)(void)); /* active prio (may be boosted) */

/* Time */
uint32_t os_get_tick(void);
uint32_t os_get_us(void);
uint32_t os_get_ms(void);

/* Errors */
uint32_t os_get_error_count(void);
uint32_t os_get_expected_error_count(void);
uint32_t os_get_unexpected_error_count(void);
uint32_t os_in_safe(void);
void os_error_expect_begin(void);
void os_error_expect_end(void);

/* Version */
uint32_t os_get_version(void);
const char* os_get_version_string(void);

/* ISR Handlers */
void OS_PendSV_Handler(void);
void OS_Fault_Handler(void);
void OS_Fault_C_Handler(void);

#ifdef __cplusplus
}
#endif


/* ============================================================================
 *  C++ API — only available from C++ compilation units
 * ============================================================================ */

#ifdef __cplusplus


/* ── Error Codes ── */

enum class OSError : uint8_t {
    NONE              = 0,
    SAFE_DELAY_MS     = 1,
    SAFE_YIELD        = 2,
    SAFE_EVENT_WAIT   = 3,
    STACK_OVERFLOW    = 4,
    INVALID_EVENT_ID  = 5,
    TASK_AFTER_START  = 6,
    TASK_STUCK        = 7,
    SAFE_TOO_LONG     = 8,
    HARDFAULT         = 9,
    PRIORITY_CONFLICT = 10,
    DEADLINE_MISS     = 11,
    TCB_CORRUPTED     = 12,
    SENSOR_TIMEOUT    = 13,
    SAFE_MUTEX_LOCK   = 14,
    RAM_TEST_FAIL     = 15,
    ERROR_COUNT
};


/* ── Task State ── */

enum class TaskState : uint8_t {
    INACTIVE = 0,
    READY,
    RUNNING,
    BLOCKED
};


/* ── Forward Declarations ── */

struct TCB;
struct OSErrorEntry;

enum class ErrorSeverity : uint8_t {
    INFO     = 0,
    WARNING  = 1,
    CRITICAL = 2
};


/* ── Error Reporting (C++ overload) ── */
extern "C" {
    void os_report_error(OSError code);
    OSError  os_get_last_error(void);
}


/* ── Monitor ── */
#if OS_MONITOR_DEADLINE || OS_MONITOR_TCB_INTEGRITY || OS_MONITOR_ERROR_LOG

/* One entry of the per-task stack usage report (os_get_stack_report) */
typedef struct {
    const char* name;        /* task name ("idle" for the idle task) */
    uint8_t     id;          /* task ID */
    uint32_t    size_bytes;  /* total stack size */
    uint32_t    peak_bytes;  /* peak usage since last start/reset */
} os_stack_report_entry_t;

extern "C" {
    uint8_t  os_task_get_state(void(*entry)(void));
    uint8_t  os_get_cpu_usage(void);
    uint8_t  os_get_cpu_usage_total(void);
    uint32_t os_get_wdg_reset_count(void);
    uint32_t os_get_stack_recovery_count(void);
    uint32_t os_get_stack_usage(void(*entry)(void));
    uint8_t  os_get_task_cpu_usage(void(*entry)(void));
    uint32_t os_get_stack_watermark(uint8_t task_id);
    uint32_t os_get_stack_watermark_percent(uint8_t task_id);
    uint8_t  os_get_stack_report_count(void);
    bool     os_get_stack_report(uint8_t index, os_stack_report_entry_t* out);
}
#endif


/* ============================================================================
 *  Task Control Block
 *  ---------------------------------------------------------------------------
 *  Memory layout verified by static_assert against assembly offsets.
 *  DO NOT reorder fields — assembly code depends on exact byte offsets.
 *
 *  Fields after cpu_ticks are optional extensions that do NOT affect
 *  assembly offsets and can be freely added/removed.
 * ============================================================================ */

struct TCB {
    /* --- Stack (offsets 0-15) --- */
    uint32_t*   stack_top;          /* [0]  Current PSP */
    uint32_t*   stack_base;         /* [4]  Bottom of stack buffer */
    uint32_t    stack_size;         /* [8]  Stack size in words */
    TaskState   state;              /* [12] Current state */
    uint8_t     priority;           /* [13] Active priority (may be PI-boosted) */
    uint8_t     id;                 /* [14] Unique task ID */
    uint8_t     wait_result;        /* [15] 0=timeout, 1=success */

    /* --- Scheduling (offsets 16-43) --- */
    const char* name;               /* [16] Debug name */
    void(*entry)(void);             /* [20] Task function */
    uint32_t    period_ticks;       /* [24] Periodic interval (0=aperiodic) */
    uint32_t    next_run_time;      /* [28] Next allowed execution */
    uint32_t    delay_ticks;        /* [32] Remaining delay */
    void*       blocking_on;        /* [36] Event/mutex causing block */
    uint32_t    block_timeout;      /* [40] Remaining timeout */

    /* --- Links (offsets 44-59) --- */
    TCB*        next;               /* [44] Global task list */
    uint32_t    last_yield_tick;    /* [48] For watchdog/deadline */
    uint8_t     mutex_nesting;      /* [52] Nested lock count */
    uint8_t     base_priority;      /* [53] Original priority (before PI) */
    uint8_t     wdg_retries;        /* [54] Watchdog recovery count */
    uint32_t    cpu_ticks;          /* [56] CPU time accounting */
    TCB*        queue_next;         /* [60] Priority queue linked list */
#if OS_SMP_CORES > 1
    uint8_t     core_id;            /* [64] Assigned core (0=any, 1=core0, 2=core1) */
    uint8_t     _pad[3];            /* [65] Alignment padding */
#endif
#if OS_MONITOR_ENABLED
    uint32_t*   peak_sp;            /* Lowest SP observed (watermark) */
#endif

    /* --- Optional Extensions (after assembly-critical fields) --- */
#if OS_SAFETY_MPU
    struct MPURegion {
        uint32_t base_address;
        uint32_t size;
        uint32_t attributes;
    };
    MPURegion   mpu_regions[OS_MPU_MAX_REGIONS];
    uint8_t     mpu_region_count;
#endif
#if OS_MONITOR_DEADLINE
    uint32_t    deadline_ticks;     /* Hard deadline (0=disabled) */
    uint32_t    deadline_miss_count;
#endif
#if OS_MONITOR_TCB_INTEGRITY
    uint32_t    magic;              /* Must be OS_TCB_MAGIC */
    uint32_t    overflow_count;
#endif
};


/* ============================================================================
 *  Assembly Offsets (static_assert verified — DO NOT CHANGE)
 * ============================================================================ */

#define OS_OFF_STACK_TOP          0
#define OS_OFF_STACK_BASE         4
#define OS_OFF_STACK_SIZE         8
#define OS_OFF_STATE             12
#define OS_OFF_PRIORITY          13
#define OS_OFF_ID                14
#define OS_OFF_WAIT_RESULT       15
#define OS_OFF_NAME              16
#define OS_OFF_ENTRY             20
#define OS_OFF_PERIOD_TICKS      24
#define OS_OFF_NEXT_RUN_TIME     28
#define OS_OFF_DELAY_TICKS       32
#define OS_OFF_BLOCKING_ON       36
#define OS_OFF_BLOCK_TIMEOUT     40
#define OS_OFF_NEXT              44
#define OS_OFF_LAST_YIELD_TICK   48
#define OS_OFF_MUTEX_NESTING     52
#define OS_OFF_BASE_PRIORITY     53
#define OS_OFF_WDG_RETRIES       54
#define OS_OFF_CPU_TICKS         56

static_assert(offsetof(TCB, stack_top)       == OS_OFF_STACK_TOP,       "TCB layout");
static_assert(offsetof(TCB, stack_base)      == OS_OFF_STACK_BASE,      "TCB layout");
static_assert(offsetof(TCB, stack_size)      == OS_OFF_STACK_SIZE,      "TCB layout");
static_assert(offsetof(TCB, state)           == OS_OFF_STATE,           "TCB layout");
static_assert(offsetof(TCB, priority)        == OS_OFF_PRIORITY,        "TCB layout");
static_assert(offsetof(TCB, id)              == OS_OFF_ID,              "TCB layout");
static_assert(offsetof(TCB, wait_result)     == OS_OFF_WAIT_RESULT,     "TCB layout");
static_assert(offsetof(TCB, name)            == OS_OFF_NAME,            "TCB layout");
static_assert(offsetof(TCB, entry)           == OS_OFF_ENTRY,           "TCB layout");
static_assert(offsetof(TCB, period_ticks)    == OS_OFF_PERIOD_TICKS,    "TCB layout");
static_assert(offsetof(TCB, next_run_time)   == OS_OFF_NEXT_RUN_TIME,   "TCB layout");
static_assert(offsetof(TCB, delay_ticks)     == OS_OFF_DELAY_TICKS,     "TCB layout");
static_assert(offsetof(TCB, blocking_on)     == OS_OFF_BLOCKING_ON,     "TCB layout");
static_assert(offsetof(TCB, block_timeout)   == OS_OFF_BLOCK_TIMEOUT,   "TCB layout");
static_assert(offsetof(TCB, next)            == OS_OFF_NEXT,            "TCB layout");
static_assert(offsetof(TCB, last_yield_tick) == OS_OFF_LAST_YIELD_TICK, "TCB layout");
static_assert(offsetof(TCB, mutex_nesting)   == OS_OFF_MUTEX_NESTING,   "TCB layout");
static_assert(offsetof(TCB, base_priority)   == OS_OFF_BASE_PRIORITY,   "TCB layout");
static_assert(offsetof(TCB, wdg_retries)     == OS_OFF_WDG_RETRIES,     "TCB layout");
static_assert(offsetof(TCB, cpu_ticks)       == OS_OFF_CPU_TICKS,       "TCB layout");
static_assert(offsetof(TCB, queue_next)      == 60,                     "TCB layout — queue_next");


/* ============================================================================
 *  Deadline API (optional)
 * ============================================================================ */

#if OS_MONITOR_DEADLINE
extern "C" {
    uint32_t os_get_deadline_miss_count(void(*entry)(void));
}
#endif


/* ============================================================================
 *  Error Log API (optional)
 * ============================================================================ */

#if OS_MONITOR_ERROR_LOG

struct OSErrorEntry {
    uint32_t      timestamp_tick;
    OSError       code;
    uint8_t       task_id;
    ErrorSeverity severity;
};

extern "C" {
    void os_log_error(OSError code, uint8_t severity);
    OSErrorEntry os_get_error_log_entry(uint32_t index);
    uint32_t os_get_error_log_count(void);
    uint32_t os_get_error_log_total(void);
}
#endif


/* ============================================================================
 *  Hardware Watchdog API (optional)
 * ============================================================================ */

#if OS_SAFETY_HW_WATCHDOG
extern "C" {
    void os_hw_watchdog_feed(void);
    void os_hw_watchdog_check(void);   /* feed if healthy, else skip */
    uint32_t os_get_hw_wdg_reset_count(void);
}
#endif


/* ============================================================================
 *  RAM Test API (optional)
 * ============================================================================ */

#if OS_SAFETY_RAM_TEST
extern "C" {
    void os_ram_test_step(void);
    uint8_t os_ram_test_progress(void);
    uint32_t os_get_ram_test_error_count(void);
    bool os_ram_test_complete(void);
}
#endif


/* ============================================================================
 *  MPU API (optional)
 * ============================================================================ */

#if OS_SAFETY_MPU
extern "C" {
    void os_mpu_init(void);
    void os_mpu_configure_task(void* tcb_ptr);
    void os_mpu_add_region(void* tcb_ptr, uint32_t base, uint32_t size, uint32_t attrs);
    void os_mpu_disable(void);
    void os_mpu_enable(void);
}
#endif


/* ============================================================================
 *  CRC Program Flow Monitoring API (optional)
 *  ---------------------------------------------------------------------------
 *  CubeMX enables the CRC peripheral. The OS uses it to verify ROM
 *  integrity. Call os_crc_check_step() from idle or a low-priority task.
 * ============================================================================ */

#if OS_SAFETY_CRC_CHECK
extern "C" {
    void     os_crc_init(void);          /* compute expected CRC at boot */
    void     os_crc_check_step(void);    /* check one block per call */
    uint8_t  os_crc_check_progress(void); /* 0-100 */
    bool     os_crc_check_complete(void);
    uint32_t os_get_crc_error_count(void);
}
#endif


/* ============================================================================
 *  SMP API (optional)
 * ============================================================================ */

#if OS_SMP_CORES > 1
extern "C" {
    uint8_t  os_get_core_id(void);        /* Get current core ID (0 or 1) */
    uint8_t  os_get_core_count(void);     /* Get number of active cores */
    void     os_task_set_core(void(*entry)(void), uint8_t core); /* Pin task to core */
    void     os_task_migrate(void(*entry)(void), uint8_t core);  /* Migrate task to core */
}
#endif


/* ============================================================================
 *  Task Create (Internal)
 * ============================================================================ */

extern "C" int8_t _os_task_create_internal(
    TCB* task,
    uint32_t* stack_mem,
    uint32_t stack_size,
    const char* name,
    void(*entry)(void),
    uint8_t priority,
    uint32_t period_ms);


/* ============================================================================
 *  Task Creation (Template — zero-overhead, type-safe)
 *  ---------------------------------------------------------------------------
 *  Usage:
 *      os_task_create(task_led, 5, 500);       // priority 5, 500ms period
 *      os_task_create_st(task_main, 10, 0, 256); // custom stack size
 * ============================================================================ */

template <void(*Entry)(void), uint32_t StackBytes = OS_KERNEL_STACK_SIZE>
inline int8_t _os_task_create_impl(const char* name,
    uint8_t priority = 1, uint32_t period_ms = 0)
{
#if OS_SAFETY_MPU
	/* MPU stack region needs a 32B-aligned base (see os_mpu_configure_task) */
	static uint32_t stack_raw[(StackBytes / 4) + 8] __attribute__((aligned(32)));
#else
	static uint32_t stack_raw[(StackBytes / 4) + 8] __attribute__((aligned(8)));
#endif
    static TCB tcb;
    static bool created = false;
    if (created) return -1;
    created = true;

    uintptr_t addr = (uintptr_t)stack_raw;
    addr = (addr + 7UL) & ~7UL;
    uint32_t* aligned_stack = (uint32_t*)addr;
    uint32_t aligned_size =
        (uint32_t)(((uintptr_t)stack_raw + sizeof(stack_raw) - addr) / 4UL);

    return _os_task_create_internal(
        &tcb, aligned_stack, aligned_size, name, Entry, priority, period_ms);
}

#define os_task_create(entry, ...) \
    _os_task_create_impl<entry>(#entry, ##__VA_ARGS__)

#define os_task_create_st(entry, prio, period, stack_Bytes) \
    _os_task_create_impl<entry, stack_Bytes>(#entry, prio, period)


/* ============================================================================
 *  Deadline Setter (post-creation)
 * ============================================================================ */

#if OS_MONITOR_DEADLINE
extern "C" {
    void os_task_set_deadline_raw(void(*entry)(void), uint32_t deadline_ms);
}

#define os_task_set_deadline(entry, deadline_ms) \
    os_task_set_deadline_raw(entry, deadline_ms)
#endif


/* ============================================================================
 *  RAII Classes
 *  ---------------------------------------------------------------------------
 *  OS_SAFE   — interrupt-safe critical section
 *  OS_LOCK   — mutex lock guard
 *  OS_EVENT  — inter-task signaling
 *  OS_MUTEX  — mutual exclusion with priority inheritance
 * ============================================================================ */

/* --- OS_SAFE --- */
class _OsSafeGuard {
    uint32_t saved_primask;
    uint32_t start_cycle;
    bool done;
public:
    _OsSafeGuard();
    ~_OsSafeGuard();
    bool once();
    _OsSafeGuard(const _OsSafeGuard&) = delete;
    _OsSafeGuard& operator=(const _OsSafeGuard&) = delete;
};

#define OS_SAFE for (_OsSafeGuard _os_s; _os_s.once(); )


/* --- OS_EVENT --- */
#if OS_TOOL_EVENT

struct ECB { volatile uint32_t count; volatile uint32_t in_use; int16_t id; ECB* next; };

extern "C" {
    void os_event_signal(int16_t id);
    void os_event_signal_from_isr(uint32_t mask);
}

extern "C" {
    void    os_event_register(ECB* e);
    void    os_event_unregister(ECB* e);
    void    os_event_destroy(int16_t id);
    int     os_event_wait(int16_t id, uint32_t timeout_ms);
    extern int16_t os_event_next_id;
}

class OS_EVENT {
public:
    ECB ecb;
    int16_t id;     /* -1 = unregistered; IDs auto-assigned, no practical limit */
    OS_EVENT();
    ~OS_EVENT();
    void destroy();
    /* Mask-based signaling only covers IDs 0..31 (32-bit mask).
       Events with ID >= 32 are fully supported but must be signaled
       individually:
           ev.signal();          // task context
           ev.signal_from_isr(); // ISR context (falls back safely)
       Combining an ID >= 32 with | would not fit in the mask. */
    void signal(uint32_t mask = 0);
    void signal_from_isr(uint32_t mask = 0);
    int wait(uint32_t timeout_ms = OS_WAIT_FOREVER);
    OS_EVENT(const OS_EVENT&) = delete;
    OS_EVENT& operator=(const OS_EVENT&) = delete;
};

static inline uint32_t operator|(const OS_EVENT& a, const OS_EVENT& b) {
    /* Guard against undefined shifts: only IDs 0..31 fit in a 32-bit mask */
    uint32_t m = 0;
    if (a.id >= 0 && a.id < 32) m |= (1UL << a.id);
    if (b.id >= 0 && b.id < 32) m |= (1UL << b.id);
    return m;
}

#endif /* OS_TOOL_EVENT */


/* --- OS_MUTEX --- */
#if OS_TOOL_MUTEX

extern "C" {
    TCB* os_get_current_task(void);
    TCB* os_mutex_handoff(void* mutex_obj);
    void os_mutex_block_on(void* obj, uint32_t timeout_ticks);
    int  os_mutex_check_and_clear_result(void);
}

class OS_MUTEX {
    volatile uint8_t locked;
    TCB* owner;
    uint8_t ceiling_priority;   /* IPC: highest priority of any user */
    bool priority_boosted;
public:
    OS_MUTEX(uint8_t ceiling = 0);
    bool lock(uint32_t timeout_ms = OS_WAIT_FOREVER);
    void unlock();
    bool is_locked() const;
    void set_ceiling(uint8_t prio);
    OS_MUTEX(const OS_MUTEX&) = delete;
    OS_MUTEX& operator=(const OS_MUTEX&) = delete;
};

class _OsLockGuard {
    OS_MUTEX& mtx;
    bool acquired;
    mutable bool done;
public:
    _OsLockGuard(OS_MUTEX& m, uint32_t timeout_ms = OS_WAIT_FOREVER);
    ~_OsLockGuard();
    bool once();
    _OsLockGuard(const _OsLockGuard&) = delete;
    _OsLockGuard& operator=(const _OsLockGuard&) = delete;
};

#define OS_LOCK(mtx) for (_OsLockGuard _os_l(mtx); _os_l.once(); )

#endif /* OS_TOOL_MUTEX */


/* ============================================================================
 *  OS_QUEUE — Bounded FIFO for Inter-Task Data Transfer
 * ============================================================================ */

#if OS_TOOL_QUEUE

template <typename T, uint32_t Capacity>
class OS_QUEUE {
    T        buf[Capacity];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    OS_MUTEX mtx;
    OS_EVENT not_full;
    OS_EVENT not_empty;

public:
    OS_QUEUE() : head(0), tail(0), count(0) {}
    ~OS_QUEUE() {} // OS_EVENT destructors auto-destroy events

    bool put(const T& item) { return put(item, OS_WAIT_FOREVER); }

    bool put(const T& item, uint32_t timeout_ms) {
        while (1) {
            OS_LOCK(mtx) {
                if (count < Capacity) {
                    buf[head] = item;
                    head = (head + 1) % Capacity;
                    count++;
                    not_empty.signal();
                    return true;
                }
            }
            if (!not_full.wait(timeout_ms)) return false;
        }
    }

    bool put_from_isr(const T& item) {
        uint32_t cs = os_critical_enter();
        if (count >= Capacity) { os_critical_exit(cs); return false; }
        buf[head] = item;
        head = (head + 1) % Capacity;
        count++;
        os_critical_exit(cs);
        not_empty.signal_from_isr();
        return true;
    }

    bool get(T& item) { return get(item, OS_WAIT_FOREVER); }

    bool get(T& item, uint32_t timeout_ms) {
        while (1) {
            OS_LOCK(mtx) {
                if (count > 0) {
                    item = buf[tail];
                    tail = (tail + 1) % Capacity;
                    count--;
                    not_full.signal();
                    return true;
                }
            }
            if (!not_empty.wait(timeout_ms)) return false;
        }
    }

    uint32_t get_count()    const { return count; }
    bool     is_full()      const { return count >= Capacity; }
    bool     is_empty()     const { return count == 0; }
    uint32_t get_capacity() const { return Capacity; }

    void reset() {
        OS_LOCK(mtx) { head = tail = count = 0; }
    }

    OS_QUEUE(const OS_QUEUE&) = delete;
    OS_QUEUE& operator=(const OS_QUEUE&) = delete;
};

#endif /* OS_TOOL_QUEUE */


/* ============================================================================
 *  OS_SEMAPHORE — Counting Semaphore
 * ============================================================================ */

#if OS_TOOL_SEMAPHORE

class OS_SEMAPHORE {
    uint32_t count;
    uint32_t max_count;
    OS_MUTEX mtx;
    OS_EVENT has_count;

public:
    OS_SEMAPHORE(uint32_t initial = 1, uint32_t max = 0)
        : count(initial), max_count(max ? max : 0xFFFFFFFFUL) {}
    ~OS_SEMAPHORE() {} // OS_EVENT destructor auto-destroys event

    bool wait() { return wait(OS_WAIT_FOREVER); }

    bool wait(uint32_t timeout_ms) {
        while (1) {
            OS_LOCK(mtx) {
                if (count > 0) { count--; return true; }
            }
            if (!has_count.wait(timeout_ms)) return false;
        }
    }

    bool signal() {
        OS_LOCK(mtx) {
            if (count < max_count) { count++; has_count.signal(); return true; }
        }
        return false;
    }

    bool signal_from_isr() {
        uint32_t cs = os_critical_enter();
        if (count < max_count) {
            count++;
            os_critical_exit(cs);
            has_count.signal_from_isr();
            return true;
        }
        os_critical_exit(cs);
        return false;
    }

    uint32_t get_count()     const { return count; }
    uint32_t get_max_count() const { return max_count; }
    bool     is_empty()      const { return count == 0; }
    bool     is_full()       const { return count >= max_count; }

    void reset(uint32_t new_count) {
        OS_LOCK(mtx) { count = (new_count <= max_count) ? new_count : max_count; }
    }

    OS_SEMAPHORE(const OS_SEMAPHORE&) = delete;
    OS_SEMAPHORE& operator=(const OS_SEMAPHORE&) = delete;
};

#endif /* OS_TOOL_SEMAPHORE */

#endif /* __cplusplus */
