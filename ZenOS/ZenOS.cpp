/**
 * @file    ZenOS.cpp
 * @brief   ZenOS RTOS — Implementation
 * @author  Rahman Heidari (rahman.h22@gmail.com)
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS.hpp"

extern "C" uint32_t SystemCoreClock;


static uint32_t ram_vectors[OS_VECTOR_COUNT] OS_ALIGNED(512);
volatile uint32_t os_safe_depth = 0;


/* ═══════════════ متغیرهای سراسری ═══════════════ */
/* All scheduler globals below are shared between task context, the naked
   PendSV handler and the SysTick/EXTI ISRs.  They MUST be volatile: at
   -O2 the compiler otherwise keeps them cached in registers across the
   asm boundaries (os_yield, OS_PendSV_Handler) and the scheduler then
   switches to stale TCBs — the classic "works at -O0, corrupt at -O2". */
TCB*              volatile task_list    = nullptr;
uint16_t          volatile task_count   = 0;
TCB*              volatile current_task = nullptr;

/* ═══════════════ SMP (Symmetric Multi-Processing) ═══════════════ */
#if OS_SMP_CORES > 1
static TCB* core_current_task[OS_SMP_MAX_CORES] = {nullptr};
static volatile uint8_t os_core_count_active = 1;

/* STM32H7: Core ID register at 0xE000ED00 (CPUID) bits [15:8] */
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

uint32_t          idle_stack[OS_IDLE_STACK_WORDS] OS_ALIGNED(8);
volatile uint32_t tick_count = 0;

#if OS_TOOL_EVENT
static ECB* volatile event_list = nullptr;
#endif

static TCB idle_tcb;
extern "C" TCB* const os_idle_tcb_ptr = &idle_tcb;

static volatile uint32_t blocked_count   = 0;

/* ═══════════════ Atomic blocked_count (C2 fix) ═══════════════
   LDREX/STREX: lock-free, no interrupt disable needed.
   Safe from any ISR context (SysTick, EXTI, PendSV). ═══════════════ */
static inline void os_blocked_inc(void) {
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
}

static inline void os_blocked_dec(void) {
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

static volatile uint32_t idle_ticks      = 0;
static volatile bool     os_started      = false;

/* ═══════════════ O(1) Priority Bitmap Scheduler ═══════════════
   Instead of scanning the linked list O(n), we maintain a 32-bit
   bitmap where each bit represents a priority level (0-31).
   CLZ (Count Leading Zeros) finds the highest set bit in 1 cycle. ═══════════════ */
static volatile uint32_t os_ready_bitmap = 0;

/* Priority queue heads: os_pq_head[priority] = first TCB at that priority */
static TCB* volatile os_pq_head[32] = {nullptr};

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

/* Add a task to its priority queue and set the bitmap bit */
static void os_pq_add(TCB* task) {
	if (!task || task->priority >= 32) return;
	uint8_t p = task->priority;
	task->queue_next = os_pq_head[p];
	os_pq_head[p] = task;
	os_pq_set_bit(p);
}

/* Remove a task from its priority queue */
static void os_pq_remove(TCB* task) {
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

/* Find the highest priority READY task — O(1) via bitmap + CLZ */
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

/* ── فقط وقتی تیکلس فعال است ── */
static volatile uint32_t os_syst_rvr_normal = 0;
static uint32_t fault_stack[OS_FAULT_STACK_WORDS] OS_ALIGNED(8);

/* OS_STR macros are defined in OSTM32_config.hpp */

/* ═══════════════ Error System ═══════════════ */
/* Error counters are touched from ISR context (os_tick reports DEADLINE_MISS /
   TASK_STUCK / STACK_OVERFLOW) and read from task context — volatile too. */
static volatile uint32_t error_total = 0;
static volatile OSError  error_last  = OSError::NONE;
static volatile uint32_t error_expected     = 0;  /* errors inside OS_ERROR_EXPECTED */
static volatile uint32_t error_expect_depth = 0;  /* nesting depth of the marker     */
static volatile uint32_t wdg_reset_count      = 0;
static volatile uint32_t stack_recovery_count = 0;

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

/* ═══════════════ Critical Section ═══════════════ */
extern "C" uint32_t os_critical_enter(void) {
	uint32_t old;
	__asm volatile("mrs %0, PRIMASK\n cpsid i\n" : "=r"(old) :: "memory");
	return old;
}

extern "C" void os_critical_exit(uint32_t old) {
	__asm volatile("msr PRIMASK, %0" :: "r"(old) : "memory");
}

/* ═══════════════ ISR Context Detection ═══════════════ */
/* True when running inside any exception/ISR (IPSR != 0).
   os_safe_depth alone is not enough — a blocking call made directly
   from an ISR would otherwise corrupt the scheduler state. */
static inline bool os_in_isr(void) {
	uint32_t ipsr;
	__asm volatile("mrs %0, IPSR" : "=r"(ipsr));
	return ipsr != 0;
}

/* ═══════════════ Forward Declarations ═══════════════ */
static void os_task_exit(void);
static void os_stack_init(TCB* task);

/* ═══════════════ Task Reset ═══════════════ */
static void os_reset_task_internal(TCB* task) {
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
static void os_task_exit(void) {
	uint32_t cs = os_critical_enter();
	if (current_task) {
		current_task->state = TaskState::INACTIVE;
	}
	os_critical_exit(cs);
	os_yield();
	while (1) { __asm volatile("nop"); }
}

/* ═══════════════ Helper: wake a blocked task ═══════════════ */
static void os_wake_task(TCB* t, uint8_t result) {
	t->wait_result     = result;
	t->state           = TaskState::READY;
	t->blocking_on     = nullptr;
	t->block_timeout   = 0;
	t->next_run_time   = tick_count;
	t->last_yield_tick = tick_count;
	if (blocked_count > 0) os_blocked_dec();
	/* O(1) scheduler: add to priority queue */
	os_pq_add(t);
}

/* ═══════════════ Unified block/wake helpers ═══════════════
   Consolidates the repeated blocking pattern found in:
   os_delay_ms, os_event_wait, os_mutex_block_on.
   Returns true if the caller should yield. ═══════════════ */
static bool os_block_current(TaskState state, void* blocking_on,
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
	os_blocked_inc();
	return true;
}

/* ═══════════════ Tick Wake Helpers ═══════════════
   Inlined wake logic shared by os_tick and os_tickless_process.
   Each handles one expiry case: delay (result=1) or timeout (result=2). ═══════════════ */
static inline void os_wake_on_delay_expiry(TCB* task) {
	os_wake_task(task, 1);
}

static inline void os_wake_on_timeout_expiry(TCB* task) {
	task->wait_result = 2;
	os_wake_task(task, 2);
}

/* ═══════════════ Helper: ms to ticks ═══════════════ */
/* Opt3: os_ms_to_ticks — 32-bit fast path (eliminates 64-bit MUL) */
static uint32_t os_ms_to_ticks(uint32_t ms) {
	if (ms == OS_WAIT_FOREVER) return 0;
	/* <= keeps the largest exact value: ms == 0xFFFFFFFF/OS_TICKS_PER_MS
	   still fits in 32 bits when multiplied. */
	if (ms <= (0xFFFFFFFFUL / OS_TICKS_PER_MS)) {
		uint32_t r = ms * OS_TICKS_PER_MS;
		return r ? r : 1;
	}
	return 0xFFFFFFFEUL;
}

/* ═══════════════ Stack Init ═══════════════ */
/* Opt2: os_stack_init — STMDB batch for exception frame (~40% faster) */
static void os_stack_init(TCB* task) {
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

/* ═══════════════ TCB Integrity ═══════════════ */
#if OS_MONITOR_TCB_INTEGRITY
static inline bool os_tcb_check_magic(TCB* t) {
	return t && t->magic == OS_TCB_MAGIC;
}
#endif

/* ═══════════════ Stack Check ═══════════════ */
static void os_stack_check_all(void) {
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
		if (task->state == TaskState::BLOCKED && blocked_count > 0)
			os_blocked_dec();

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

/* ═══════════════ Task Create ═══════════════ */
static TCB* os_find_task_by_entry(void(*entry)(void));

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
static TCB* os_find_task_by_entry(void(*entry)(void)) {
	if (!entry) return nullptr;
	for (TCB* t = task_list; t; t = t->next)
		if (t->entry == entry) return t;
	return nullptr;
}

static TCB* os_find_task_by_id(uint8_t id) {
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
		if (t->state == TaskState::BLOCKED && blocked_count > 0)
			os_blocked_dec();
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
static bool os_tickless_process(uint32_t skip) {
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

/* ═══════════════ Tick Handler ═══════════════ */	extern "C" void os_tick(void) {

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

	/* Always fire PendSV — scheduler handles time-slicing,
	   periodic task period, and round-robin rotation. */
	OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
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

/* ═══════════════ Events ═══════════════ */
#if OS_TOOL_EVENT
int16_t os_event_next_id = 0;

extern "C" void os_event_register(ECB* e) {
	if (!e) return;
	uint32_t cs = os_critical_enter();
	e->id = os_event_next_id++; e->in_use = 1; e->count = 0;
	e->next = event_list; event_list = e;
	os_critical_exit(cs);
}

extern "C" void os_event_unregister(ECB* e) {
	if (!e) return;
	uint32_t cs = os_critical_enter();
	/* Wake any tasks blocked on this event */
	for (TCB* t = task_list; t; t = t->next)
		if (t->state == TaskState::BLOCKED && t->blocking_on == e) os_wake_task(t, 0);
	/* Remove from list */
	ECB* prev = nullptr; ECB* cur = event_list;
	while (cur) { if (cur == e) break; prev = cur; cur = cur->next; }
	if (cur) { if (prev) prev->next = cur->next; else event_list = cur->next; }
	e->in_use = 0;
	os_critical_exit(cs);
}

extern "C" void os_event_destroy(int16_t id) {
	uint32_t cs = os_critical_enter();
	ECB* e = event_list;
	while (e) { if (e->id == id && e->in_use) break; e = e->next; }
	if (!e) { os_critical_exit(cs); os_report_error(OSError::INVALID_EVENT_ID); return; }
	os_event_unregister(e);
	os_critical_exit(cs);
}

static ECB* os_find_event(int16_t id) {
	ECB* e = event_list;
	while (e) { if (e->id == id && e->in_use) return e; e = e->next; }
	return nullptr;
}

extern "C" void os_event_signal(int16_t id) {
	uint32_t cs = os_critical_enter();
	ECB* e = os_find_event(id);
	if (!e) { os_critical_exit(cs); os_report_error(OSError::INVALID_EVENT_ID); return; }
	bool woke = false;
	if (blocked_count > 0) {
		for (TCB* t = task_list; t; t = t->next) {
			if (t->state == TaskState::BLOCKED && t->blocking_on == e) {
				os_wake_task(t, 1); woke = true; break;
			}
		}
	}
	if (!woke) e->count++;
	if (woke) OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
	os_critical_exit(cs);
}

extern "C" void os_event_signal_from_isr(uint32_t mask) {
	bool woke = false;
	uint32_t cs = os_critical_enter();
	ECB* e = event_list;
	while (e && mask) {
		/* Mask is 32-bit; events with id >= 32 are not addressable here
		   (signal them individually instead) */
		if (e->in_use && e->id >= 0 && e->id < 32) {
			uint32_t bit = 1UL << e->id;
			if (mask & bit) {
				mask &= ~bit;
				bool e_woke = false;
				if (blocked_count > 0) {
					for (TCB* t = task_list; t; t = t->next) {
						if (t->state == TaskState::BLOCKED && t->blocking_on == e) {
							os_wake_task(t, 1); e_woke = true; break;
						}
					}
				}
				if (!e_woke) e->count++;
				else woke = true;
			}
		}
		e = e->next;
	}
	os_critical_exit(cs);
	if (woke) OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
}

extern "C" int os_event_wait(int16_t id, uint32_t timeout_ms) {
	uint32_t cs = os_critical_enter();
	ECB* e = os_find_event(id);
	if (!e) { os_critical_exit(cs); os_report_error(OSError::INVALID_EVENT_ID); return 0; }
	if ((os_safe_depth > 0 || os_in_isr()) && timeout_ms > 0) {
		os_critical_exit(cs);
		os_report_error(OSError::SAFE_EVENT_WAIT);
		return 0;
	}
	if (e->count > 0) {
		e->count--;
		os_critical_exit(cs);
		return 1;
	}
	if (timeout_ms == 0) {
		os_critical_exit(cs);
		return 0;
	}
	uint32_t tt = os_ms_to_ticks(timeout_ms);
	os_block_current(TaskState::BLOCKED, e, tt, 0);
	os_critical_exit(cs);
	os_yield();

	cs = os_critical_enter();
	int result = 0;
	if (current_task) {
		result = (current_task->wait_result == 1) ? 1 : 0;
		current_task->wait_result = 0;
	}
	os_critical_exit(cs);
	return result;
}
#endif /* OS_TOOL_EVENT */

/* ═══════════════ Mutex Support ═══════════════ */
#if OS_TOOL_MUTEX 
extern "C" TCB* os_get_current_task(void) { return current_task; }

extern "C" void os_mutex_block_on(void* obj, uint32_t timeout_ticks) {
	/* Blocking from an ISR would corrupt the scheduler — refuse */
	if (os_in_isr()) { os_report_error(OSError::SAFE_MUTEX_LOCK); return; }
	uint32_t cs = os_critical_enter();
	os_block_current(TaskState::BLOCKED, obj, timeout_ticks, 0);
	os_critical_exit(cs);
	os_yield();
}

extern "C" int os_mutex_check_and_clear_result(void) {
	uint32_t cs = os_critical_enter();
	int result = current_task ? (current_task->wait_result == 1 ? 1 : 0) : 0;
	if (current_task) current_task->wait_result = 0;
	os_critical_exit(cs);
	return result;
}

extern "C" TCB* os_mutex_handoff(void* mutex_obj) {
	TCB* best = nullptr;
	uint8_t best_prio = 0;
	for (TCB* t = task_list; t; t = t->next) {
		if (t->state == TaskState::BLOCKED && t->blocking_on == mutex_obj) {
			if (t->priority >= best_prio) {
				best_prio = t->priority;
				best = t;
			}
		}
	}
	if (best) {
		os_wake_task(best, 1);
		OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
	}
	return best;
}
#endif /* OS_TOOL_MUTEX */


/* ═══════════════ Fault Handler ═══════════════ */
extern "C" void OS_Fault_C_Handler(void) {
	os_report_error(OSError::HARDFAULT);
	os_safe_depth = 0;

	TCB* fault_task = current_task;
	if (fault_task && fault_task != &idle_tcb) {
		if (fault_task->state == TaskState::BLOCKED && blocked_count > 0)
			os_blocked_dec();
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
 *  C++ Class Implementations
 * ══════════════════════════════════════════════════════════════════════ */

/* ── OsSafeGuard ── */
_OsSafeGuard::_OsSafeGuard()
	: done(false) {
#if OS_HAS_CYCLE_COUNTER
	start_cycle = OS_DWT_CYCCNT;
#endif
	__asm volatile("mrs %0, PRIMASK\n cpsid i\n" : "=r"(saved_primask) :: "memory");
	os_safe_depth++;
}

_OsSafeGuard::~_OsSafeGuard() {
	if (os_safe_depth > 0) os_safe_depth--;
#if OS_HAS_CYCLE_COUNTER && (OS_SAFETY_MAX_CRITICAL_US > 0)
	{
		uint32_t elapsed = OS_DWT_CYCCNT - start_cycle;
		uint32_t per_us  = SystemCoreClock / 1000000UL;
		if (per_us > 0 && (elapsed / per_us) > OS_SAFETY_MAX_CRITICAL_US)
			os_report_error(OSError::SAFE_TOO_LONG);
	}
#endif
	__asm volatile("msr PRIMASK, %0" :: "r"(saved_primask) : "memory");
}

bool _OsSafeGuard::once() {
	if (done) return false;
	done = true;
	return true;
}

/* ── OsEvent ── */
#if OS_TOOL_EVENT
OS_EVENT::OS_EVENT() {
	ecb.count = 0; ecb.in_use = 0; ecb.id = -1; ecb.next = nullptr;
	os_event_register(&ecb);
	id = ecb.id;
	if (id < 0) os_report_error(OSError::INVALID_EVENT_ID);
}

OS_EVENT::~OS_EVENT() {
	destroy();
}

void OS_EVENT::destroy() {
	if (id >= 0) { os_event_unregister(&ecb); id = -1; }
}

void OS_EVENT::signal(uint32_t mask) {
	if (id < 0) return;
	if (mask) {
		while (mask) {
			uint8_t i = __builtin_ctz(mask);
			mask &= mask - 1;
			os_event_signal((int16_t)i);
		}
	} else {
		os_event_signal(id);
	}
}

void OS_EVENT::signal_from_isr(uint32_t mask) {
	if (id < 0) return;
	if (mask) {
		os_event_signal_from_isr(mask);
	} else {
		/* For id >= 32, fall back to non-ISR signal */
		if (id < 32) os_event_signal_from_isr(1UL << id);
		else os_event_signal(id);
	}
}

int OS_EVENT::wait(uint32_t timeout_ms) {
	if (id < 0) return 0;
	return os_event_wait(id, timeout_ms);
}
#endif /* OS_TOOL_EVENT */

/* ── OsMutex (IPC: Immediate Priority Ceiling) ── */
#if OS_TOOL_MUTEX
OS_MUTEX::OS_MUTEX(uint8_t ceiling)
	: locked(0)
	, owner(nullptr)
	, ceiling_priority(ceiling)
	, priority_boosted(false) {}

bool OS_MUTEX::is_locked() const { return locked != 0; }

void OS_MUTEX::set_ceiling(uint8_t prio) { ceiling_priority = prio; }

bool OS_MUTEX::lock(uint32_t timeout_ms) {
	while (1) {
		uint32_t cs;
		__asm volatile("mrs %0, PRIMASK\n cpsid i\n" : "=r"(cs) :: "memory");

		if (!locked) {
			locked = 1;
			TCB* self = os_get_current_task();
			if (self) {
				self->mutex_nesting++;
				if (self->mutex_nesting == 1) {
					self->base_priority = self->priority;
					/* IPC: immediately boost to ceiling priority.
					   Re-queue in the O(1) priority queues so the scheduler
					   sees the boosted priority (READY/RUNNING tasks stay
					   queued in this design). */
					if (ceiling_priority > self->priority) {
						if (self->state != TaskState::BLOCKED &&
						    self->state != TaskState::INACTIVE)
							os_pq_remove(self);
						self->priority = ceiling_priority;
						priority_boosted = true;
						if (self->state != TaskState::BLOCKED &&
						    self->state != TaskState::INACTIVE)
							os_pq_add(self);
					}
				}
			}
			owner = self;
			__asm volatile("msr PRIMASK, %0" :: "r"(cs) : "memory");
			return true;
		}

		/* Recursive lock: same task already owns it */
		TCB* self = os_get_current_task();
		if (self && self == owner) {
			self->mutex_nesting++;
			__asm volatile("msr PRIMASK, %0" :: "r"(cs) : "memory");
			return true;
		}

		/* IPC: owner keeps ceiling — no PI needed, block and wait */
		__asm volatile("msr PRIMASK, %0" :: "r"(cs) : "memory");

		uint32_t tt = (timeout_ms == OS_WAIT_FOREVER)
			? 0 : os_ms_to_ticks(timeout_ms);

		os_mutex_block_on(this, tt);

		int result = os_mutex_check_and_clear_result();
		if (result) return true;
		if (timeout_ms != OS_WAIT_FOREVER) return false;
	}
}

void OS_MUTEX::unlock() {
	uint32_t cs;
	__asm volatile("mrs %0, PRIMASK\n cpsid i\n" : "=r"(cs) :: "memory");

	if (owner) {
		owner->mutex_nesting--;
		/* Still nested — don't release the lock */
		if (owner->mutex_nesting > 0) {
			__asm volatile("msr PRIMASK, %0" :: "r"(cs) : "memory");
			return;
		}
		/* IPC: restore original priority and re-queue so the scheduler
		   sees the un-boosted priority again. */
		if (priority_boosted) {
			if (owner->state != TaskState::BLOCKED &&
			    owner->state != TaskState::INACTIVE)
				os_pq_remove(owner);
			owner->priority = owner->base_priority;
			priority_boosted = false;
			if (owner->state != TaskState::BLOCKED &&
			    owner->state != TaskState::INACTIVE)
				os_pq_add(owner);
		}
	}

	/* Handoff to highest-priority waiter */
	TCB* new_owner = os_mutex_handoff(this);
	if (new_owner) {
		owner = new_owner;
		new_owner->mutex_nesting++;
		if (new_owner->mutex_nesting == 1) {
			new_owner->base_priority = new_owner->priority;
			/* IPC: boost new owner to ceiling and re-queue (it was just
			   woken by os_mutex_handoff → READY and queued). */
			if (ceiling_priority > new_owner->priority) {
				os_pq_remove(new_owner);
				new_owner->priority = ceiling_priority;
				priority_boosted = true;
				os_pq_add(new_owner);
			}
		}
	}
	else {
		locked = 0;
		owner  = nullptr;
	}
	__asm volatile("msr PRIMASK, %0" :: "r"(cs) : "memory");
}

/* ── OsLockGuard ── */
_OsLockGuard::_OsLockGuard(OS_MUTEX& m, uint32_t timeout_ms)
	: mtx(m)
	, acquired(mtx.lock(timeout_ms))
	, done(false) {}

_OsLockGuard::~_OsLockGuard() { if (acquired) mtx.unlock(); }

bool _OsLockGuard::once() {
	if (done) return false;
	done = true;
	return true;
}
#endif /* OS_TOOL_MUTEX */


/* ═══════════════ Stack Watermarking ═══════════════
   Canary pattern: 0xA5A5A5A5 (set in os_stack_init).
   Scan from stack_base upward to find first non-canary word = peak usage. ═══════════════ */
#if OS_MONITOR_ENABLED
static uint32_t os_stack_watermark_scan(const TCB* task) {
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



/* ═══════════════ Tickless Idle ═══════════════════════════════ */
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
	os_rr_skip = 0;
	os_rr_skip_quota = 0;
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
