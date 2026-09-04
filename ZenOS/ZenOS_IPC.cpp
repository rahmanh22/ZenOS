/**
 * @file    ZenOS_IPC.cpp
 * @brief   ZenOS RTOS — IPC: Events, Mutex, C++ Wrapper Classes
 *
 * Extracted from ZenOS.cpp as part of the modular split.
 * Contains: event register/unregister/signal/wait,
 *           mutex block/handoff, OS_EVENT/OS_MUTEX/OS_LOCK_GUARD/OS_SAFE_GUARD.
 *
 * @author  Rahman Heidari (rahman.h22@gmail.com)
 * @version 1.0.0
 */

#define OS_BUILD
#include "ZenOS_Internal.hpp"

extern "C" uint32_t SystemCoreClock;

/* ═══════════════ Events ═══════════════ */
#if OS_TOOL_EVENT

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
    if (woke) {
        /* DSB: ensure os_wake_task writes (state, pq) are visible
           before PendSV reads them. Without this, the exception
           handler may see stale TCB fields at -O2. */
        __asm volatile("dsb" ::: "memory");
        OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    }
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
    if (woke) {
        __asm volatile("dsb" ::: "memory");
        OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    }
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
        /* DSB: ensure os_wake_task writes are visible before PendSV */
        __asm volatile("dsb" ::: "memory");
        OS_SCB_ICSR = OS_ICSR_PENDSVSET_Msk;
    }
    return best;
}
#endif /* OS_TOOL_MUTEX */


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
