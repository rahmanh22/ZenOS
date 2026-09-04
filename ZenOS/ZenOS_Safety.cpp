/**
 * @file    ZenOS_Safety.cpp
 * @brief   ZenOS RTOS — Safety: Error System, Stack Check, Fault Handler,
 *          Hardware Watchdog, CRC Check, RAM Test, MPU Protection
 *
 * Extracted from ZenOS.cpp as part of the modular split.
 *
 * @author  Rahman Heidari <rahman.h22@gmail.com> — Raymon Research Team
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS_Internal.hpp"

extern "C" uint32_t SystemCoreClock;

/* ═══════════════ Error System ═══════════════ */
/* Error counters are touched from ISR context (os_tick reports DEADLINE_MISS /
   TASK_STUCK / STACK_OVERFLOW) and read from task context — volatile too. */

#if OS_SAFETY_SOFT_WATCHDOG
uint32_t os_get_wdg_reset_count(void)      { return wdg_reset_count; }
uint32_t os_get_stack_recovery_count(void) { return stack_recovery_count; }
#endif

uint32_t os_get_error_count(void)            { return error_total; }
uint32_t os_get_expected_error_count(void)   { return error_expected; }
uint32_t os_get_unexpected_error_count(void) { return error_total - error_expected; }
OSError  os_get_last_error(void)             { return error_last; }
uint32_t os_in_safe(void)                    { return os_safe_depth; }

/* Mark a region whose errors are deliberate (test fault injection): errors
   reported inside are excluded from os_get_unexpected_error_count(). */
extern "C" void os_error_expect_begin(void) { error_expect_depth++; }
extern "C" void os_error_expect_end(void)   { if (error_expect_depth > 0) error_expect_depth--; }

/* Opt4: os_report_error — no CS needed (error path only, last-writer-wins) */
void os_report_error(OSError code) {
    error_total++;
    if (error_expect_depth > 0) error_expected++;
    error_last = code;
}

/* ═══════════════ Stack Check ═══════════════ */
void os_stack_check_all(void) {
    for (TCB* task = task_list; task; task = task->next) {
        if (task == &idle_tcb) continue;
        if (task->state == TaskState::INACTIVE) continue;
        if (!task->stack_base) continue;

        bool overflow = false;

        /* ── Check 1: stack pointer below canary region (real overflow) ── */
        uint32_t* min_sp = task->stack_base + OS_STACK_CANARY_COUNT;
        if (task->stack_top < min_sp) {
            overflow = true;
        }

        /* ── Check 2: canary corruption (external memory corruption) ── */
        if (!overflow) {
            for (uint32_t i = 0; i < OS_STACK_CANARY_COUNT; i++) {
                if (task->stack_base[i] != OS_STACK_CANARY) {
                    overflow = true;
                    break;
                }
            }
        }

        if (!overflow) continue;

        /* Stack overflow confirmed */
        os_report_error(OSError::STACK_OVERFLOW);

        /* O(1) scheduler: remove from priority queue before state change */
        os_pq_remove(task);
        if (task->state == TaskState::BLOCKED && blocked_count > 0) {
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

#if OS_MONITOR_TCB_INTEGRITY
        /* TCB corrupted too — too risky to reset, deactivate instead */
        if (!os_tcb_check_magic(task)) {
            task->state = TaskState::INACTIVE;
            continue;
        }
#endif

        if (task == current_task) {
            task->state  = TaskState::INACTIVE;
            current_task = nullptr;
            OS_SCB_ICSR  = OS_ICSR_PENDSVSET_Msk;
            return;
        }

        if (stack_recovery_count < OS_SAFETY_TASK_MAX_RECOVERY) {
            stack_recovery_count++;
            os_reset_task_internal(task);
        }
        else {
            task->state = TaskState::INACTIVE;
        }
    }
}

/* ═══════════════ Fault Handler ═══════════════ */
extern "C" void OS_Fault_C_Handler(void) {
    os_report_error(OSError::HARDFAULT);
    os_safe_depth = 0;

    TCB* fault_task = current_task;
    if (fault_task && fault_task != &idle_tcb) {
        if (fault_task->state == TaskState::BLOCKED && blocked_count > 0) {
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
        fault_task->state = TaskState::INACTIVE;
    }
    current_task = nullptr;

    os_stack_init(&idle_tcb);
    idle_tcb.state         = TaskState::READY;
    idle_tcb.delay_ticks   = 0;
    idle_tcb.blocking_on   = nullptr;
    idle_tcb.block_timeout = 0;
    idle_tcb.wait_result   = 0;
    idle_tcb.last_yield_tick = tick_count;
    idle_tcb.mutex_nesting = 0;
    idle_tcb.base_priority = 0;

    __asm volatile("msr psp, %0" :: "r"(idle_tcb.stack_top) : "memory");
    /* DSB: ensure idle_tcb and current_task writes are visible before PendSV */
    __asm volatile("dsb" ::: "memory");
    OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
}

extern "C" OS_NAKED OS_USED void OS_Fault_Handler(void) {
    __asm volatile(
        ".syntax unified\n"
        ".thumb\n"
        "mov r0, lr\n"
        "push {r0}\n"
        "cpsid i\n"
        "bl OS_Fault_C_Handler\n"
        "pop {r0}\n"
        "mov lr, r0\n"
        "cpsie i\n"
        "bx lr\n"
    );
}

/* ══════════════════════════════════════════════════════════════════════
 *  Error Log System
 * ══════════════════════════════════════════════════════════════════════ */
#if OS_MONITOR_ERROR_LOG
static OSErrorEntry error_log[OS_MONITOR_ERROR_LOG_SIZE] OS_ALIGNED(4);
static uint32_t error_log_head = 0;
static uint32_t error_log_count = 0;
static uint32_t error_log_total = 0;

extern "C" void os_log_error(OSError code, uint8_t severity) {
    uint32_t cs = os_critical_enter();
    OSErrorEntry* e = &error_log[error_log_head];
    e->timestamp_tick = tick_count;
    e->code = code;
    e->task_id = current_task ? current_task->id : 0xFF;
    e->severity = (ErrorSeverity)severity;
    error_log_head = (error_log_head + 1) % OS_MONITOR_ERROR_LOG_SIZE;
    if (error_log_count < OS_MONITOR_ERROR_LOG_SIZE) error_log_count++;
    error_log_total++;
    os_critical_exit(cs);
}

extern "C" OSErrorEntry os_get_error_log_entry(uint32_t index) {
    OSErrorEntry empty = {};
    if (index >= error_log_count) return empty;
    uint32_t cs = os_critical_enter();
    uint32_t pos = (error_log_head + OS_MONITOR_ERROR_LOG_SIZE - 1 - index) % OS_MONITOR_ERROR_LOG_SIZE;
    OSErrorEntry result = error_log[pos];
    os_critical_exit(cs);
    return result;
}

extern "C" uint32_t os_get_error_log_count(void) {
    return error_log_count;
}

extern "C" uint32_t os_get_error_log_total(void) {
    return error_log_total;
}
#endif /* OS_MONITOR_ERROR_LOG */


/* ══════════════════════════════════════════════════════════════════════
 *  RAM Test (Background March-C)
 * ══════════════════════════════════════════════════════════════════════ */
#if OS_SAFETY_RAM_TEST
static uint32_t* ram_test_current = nullptr;
static uint32_t  ram_test_errors = 0;
static bool      ram_test_complete_flag = false;

extern "C" void os_ram_test_step(void) {
    if (ram_test_complete_flag) return;
    if (!ram_test_current) {
        ram_test_current = (uint32_t*)OS_RAM_TEST_START;
    }
    if (ram_test_current >= (uint32_t*)OS_RAM_TEST_END) {
        ram_test_complete_flag = true;
        return;
    }
    /* March-C: read → write complement → read complement → restore */
    uint32_t val = *ram_test_current;
    *ram_test_current = ~val;
    if (*ram_test_current != ~val) {
        ram_test_errors++;
        os_report_error(OSError::RAM_TEST_FAIL);
    }
    *ram_test_current = val;
    ram_test_current++;
}

extern "C" uint8_t os_ram_test_progress(void) {
    uint32_t total = OS_RAM_TEST_END - OS_RAM_TEST_START;
    if (total == 0) return 100;
    uint32_t done = 0;
    if (ram_test_current) {
        done = (uint32_t)((uintptr_t)ram_test_current - OS_RAM_TEST_START);
        if (done > total) done = total;
    }
    return (uint8_t)((done * 100UL) / total);
}

extern "C" uint32_t os_get_ram_test_error_count(void) {
    return ram_test_errors;
}

extern "C" bool os_ram_test_complete(void) {
    return ram_test_complete_flag;
}
#endif /* OS_SAFETY_RAM_TEST */


/* ══════════════════════════════════════════════════════════════════════
 *  Hardware Watchdog (IWDG) — CubeMX configures IWDG peripheral
 *  ---------------------------------------------------------------------------
 *  This layer provides OS-level feed/check. The actual IWDG init
 *  (prescaler, reload, window) is done by CubeMX-generated code.
 *  os_hw_watchdog_feed() — unconditional feed (call when system is healthy)
 *  os_hw_watchdog_check() — conditional feed (skip if errors detected)
 * ══════════════════════════════════════════════════════════════════════ */
#if OS_SAFETY_HW_WATCHDOG
static volatile uint32_t hw_wdg_reset_count_var = 0;

extern "C" void os_hw_watchdog_feed(void) {
    /* Wait for any ongoing prescaler/reload update to finish
       (IWDG->SR bits PVU|RVU) so the feed is not dropped. */
    while (*((volatile uint32_t*)0x4000300CUL) & 0x03UL) { }
    /* Direct register access — no HAL dependency */
    *((volatile uint32_t*)0x40003000UL) = 0xAAAA;  /* IWDG->KR */
}

extern "C" void os_hw_watchdog_check(void) {
    /* Only feed if system is healthy:
       - error count below the transient-error threshold
       - scheduler is running a task */
    if (os_get_error_count() < 10 && os_get_current_task() != nullptr) {
        os_hw_watchdog_feed();
    }
    /* If not healthy: skip feed → IWDG will reset MCU */
}

extern "C" uint32_t os_get_hw_wdg_reset_count(void) {
    /* Check RCC CSR for IWDG reset flag */
    uint32_t csr = *((volatile uint32_t*)0x40021024UL);  /* RCC->CSR */
    if (csr & (1UL << 29)) {  /* IWDGUSRSTF */
        hw_wdg_reset_count_var++;
        /* Clear flag */
        *((volatile uint32_t*)0x40021024UL) |= (1UL << 24);  /* RMVF */
    }
    return hw_wdg_reset_count_var;
}
#endif /* OS_SAFETY_HW_WATCHDOG */


/* ══════════════════════════════════════════════════════════════════════
 *  CRC Program Flow Monitoring — CubeMX enables CRC peripheral
 *  ---------------------------------------------------------------------------
 *  Verifies ROM integrity by computing CRC over flash and comparing
 *  against expected value stored at boot.
 * ══════════════════════════════════════════════════════════════════════ */
#if OS_SAFETY_CRC_CHECK
static uint32_t crc_expected = 0;
static uint32_t crc_current_addr = 0;
static uint32_t crc_error_count = 0;
static bool     crc_complete = false;
static bool     crc_init_done = false;

/* Hardware CRC peripheral (STM32F1: CRC base 0x40023000, DR offset 0) */
#define HW_CRC_DR   (*((volatile uint32_t*)0x40023000UL))
#define HW_CRC_CR   (*((volatile uint32_t*)0x40023008UL))

static uint32_t os_crc_compute_block(uint32_t addr, uint32_t size_words) {
    HW_CRC_CR = 1;  /* Reset CRC */
    volatile uint32_t* p = (volatile uint32_t*)addr;
    for (uint32_t i = 0; i < size_words; i++)
        HW_CRC_DR = p[i];
    return HW_CRC_DR;
}

extern "C" void os_crc_init(void) {
    /* Compute CRC over entire flash at boot */
    crc_expected = os_crc_compute_block(OS_FLASH_START, OS_FLASH_SIZE / 4);
    crc_current_addr = OS_FLASH_START;
    crc_complete = false;
    crc_init_done = true;
}

extern "C" void os_crc_check_step(void) {
    if (!crc_init_done || crc_complete) return;

    /* Full CRC check — 64 words per step keeps the ISR budget bounded */
    uint32_t chunk_words = 64;
    uint32_t remaining_bytes = (OS_FLASH_START + OS_FLASH_SIZE) - crc_current_addr;
    uint32_t words = remaining_bytes / 4;
    if (words > chunk_words) words = chunk_words;
    crc_current_addr += words * 4;

    /* Done when the whole flash has been re-CRC'd */
    if (crc_current_addr >= OS_FLASH_START + OS_FLASH_SIZE) {
        crc_complete = true;
        uint32_t full_crc = os_crc_compute_block(OS_FLASH_START, OS_FLASH_SIZE / 4);
        if (full_crc != crc_expected) {
            crc_error_count++;
            os_report_error(OSError::HARDFAULT);  /* ROM corruption */
        }
    }
}

extern "C" uint8_t os_crc_check_progress(void) {
    if (!crc_init_done) return 0;
    if (crc_complete) return 100;
    uint32_t total = OS_FLASH_SIZE;
    uint32_t done = crc_current_addr - OS_FLASH_START;
    return (uint8_t)((done * 100UL) / total);
}

extern "C" bool os_crc_check_complete(void) {
    return crc_complete;
}

extern "C" uint32_t os_get_crc_error_count(void) {
    return crc_error_count;
}
#endif /* OS_SAFETY_CRC_CHECK */


/* ══════════════════════════════════════════════════════════════════════
 *  MPU Protection (ARM Cortex-M3+)
 * ══════════════════════════════════════════════════════════════════════ */
#if OS_SAFETY_MPU
/* CMSIS MPU registers — provided by STM32 HAL */
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t RNR;
    volatile uint32_t RBAR;
    volatile uint32_t RASR;
} MPU_Type;
#define MPU_BASE ((MPU_Type*)0xE000ED90UL)
#define MPU MPU_BASE
#define MPU_CTRL_ENABLE_Msk     (1UL)
#define MPU_CTRL_PRIVDEFENA_Msk (1UL << 2)
#define MPU_RASR_ENABLE_Pos     0
#define MPU_RASR_AP_Pos         24
#define MPU_RASR_SIZE_Pos       1
#define MPU_RASR_XN_Pos         28
/* AP values: 3 = RW for both privilege levels, 5 = RO for both */
#define MPU_AP_FULL_ACCESS  3
#define MPU_AP_READONLY     5

/* Program one MPU region. Caller must keep the MPU disabled (CTRL=0)
   while reprogramming — required by ARMv7-M. */
static void os_mpu_set_region(uint8_t region, uint32_t base,
                              uint32_t size_bytes, uint32_t ap, bool xn) {
    if (region >= OS_MPU_MAX_REGIONS) return;
    /* RASR SIZE field = log2(region_size) - 1 (region = 2^(SIZE+1) bytes) */
    uint32_t size_log = 4;
    while ((1UL << (size_log + 1)) < size_bytes && size_log < 31) size_log++;
    MPU->RNR  = region;
    MPU->RBAR = base;
    MPU->RASR = (1UL << MPU_RASR_ENABLE_Pos) |
                ((ap & 0x07UL) << MPU_RASR_AP_Pos) |
                (size_log << MPU_RASR_SIZE_Pos) |
                (xn ? (1UL << MPU_RASR_XN_Pos) : 0UL);
}

extern "C" void os_mpu_init(void) {
    MPU->CTRL = 0;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");
    /* Static regions shared by all tasks:
       0 = flash (code)  — read-only, executable
       1 = SRAM (data)   — read/write (tasks share data memory)
       2 = peripherals   — read/write, non-executable
       3 = per-task stack — programmed in os_mpu_configure_task
       4+ = extra regions via os_mpu_add_region
       PRIVDEFENA lets privileged code (kernel, idle, ISRs) bypass the MPU;
       tasks run unprivileged (CONTROL.nPRIV set in PendSV) and are
       restricted to the regions above. */
    os_mpu_set_region(0, OS_FLASH_START, OS_FLASH_SIZE, MPU_AP_READONLY, false);
    os_mpu_set_region(1, OS_RAM_START,    OS_RAM_SIZE,    MPU_AP_FULL_ACCESS, true);
    os_mpu_set_region(2, 0x40000000UL,    0x20000000UL,   MPU_AP_FULL_ACCESS, true);
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");
}

extern "C" void os_mpu_configure_task(void* tcb_ptr) {
    TCB* task = (TCB*)tcb_ptr;
    if (!task) return;
    /* Must disable the MPU before changing any region (ARMv7-M) */
    MPU->CTRL = 0;
    __asm volatile("dsb" ::: "memory");
    /* Region 3: this task's stack — read/write, non-executable */
    os_mpu_set_region(3, (uint32_t)task->stack_base & ~0x1FUL,
                      task->stack_size * 4, MPU_AP_FULL_ACCESS, true);
    /* Extra per-task regions (4+) added via os_mpu_add_region */
    for (uint8_t i = 0; i < task->mpu_region_count && (4U + i) < OS_MPU_MAX_REGIONS; i++) {
        os_mpu_set_region(4 + i, task->mpu_regions[i].base_address,
                          task->mpu_regions[i].size,
                          task->mpu_regions[i].attributes, true);
    }
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");
}

extern "C" void os_mpu_add_region(void* tcb_ptr, uint32_t base, uint32_t size, uint32_t attrs) {
    TCB* task = (TCB*)tcb_ptr;
    if (!task) return;
    if (task->mpu_region_count >= OS_MPU_MAX_REGIONS) return;
    task->mpu_regions[task->mpu_region_count].base_address = base;
    task->mpu_regions[task->mpu_region_count].size         = size;
    task->mpu_regions[task->mpu_region_count].attributes   = attrs;
    task->mpu_region_count++;
}

extern "C" void os_mpu_disable(void) {
    MPU->CTRL = 0;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");
}

extern "C" void os_mpu_enable(void) {
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");
}
#endif /* OS_SAFETY_MPU */
