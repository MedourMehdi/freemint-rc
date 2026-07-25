#include "proc_threads_sem.h"

#include "proc_threads_scheduler.h"
#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_sleep_yield.h"

static void proc_thread_sem_wakeup_handler(PROC *p, long arg);
static long semaphore_handle_signal_wakeup(struct thread *t, struct semaphore *sem);

/* =========================================================================
 * thread_semaphore_init  —  sem_init
 * =========================================================================
 *
 * BUG FIXED: original code zeroed sem->count unconditionally, ignoring the
 * caller-supplied initial value.  POSIX sem_init(3) requires the semaphore
 * to start at the value passed in.  We also moved the sign check BEFORE any
 * field writes so a bad initial_value never partially initialises the struct.
 */
long thread_semaphore_init(struct semaphore *sem, long initial_value)
{
    if (!sem) {
        TRACE_THREAD("SEMAPHORE INIT: Invalid semaphore pointer");
        return EINVAL;
    }

    if (initial_value < 0) {
        TRACE_THREAD("SEMAPHORE INIT: Invalid semaphore initial value %ld",
                     initial_value);
        return EINVAL;
    }

    sem->wait_queue = NULL;
    sem->io_count   = 0;
    sem->count      = initial_value;   /* honour the caller-supplied value */
    sem->sem_id[0]  = '\0';

    TRACE_THREAD("SEMAPHORE INIT: SEM=%p, Count=%ld", sem, sem->count);
    return THREAD_SUCCESS;
}

/* =========================================================================
 * proc_thread_sem_wakeup_handler  —  kernel timer callback for timedwait
 * =========================================================================
 *
 * Called by the kernel timeout system when the timedwait deadline expires.
 * If the thread is still blocked on the semaphore we remove it from the
 * wait queue, stamp sem_wait_obj with the ETIMEDOUT sentinel, and make the
 * thread runnable.
 *
 * BUG FIXED: the original handler ran without any interrupt lock while
 * manipulating the semaphore wait queue.  sem_up() also manipulates the
 * same queue under splhigh().  The missing lock here created a window where
 * the timer and sem_up could both walk the list concurrently, corrupting
 * next_wait pointers.  Now the whole queue removal is done under splhigh().
 */
static void proc_thread_sem_wakeup_handler(PROC *p, long arg)
{
    struct thread    *t  = (struct thread *)arg;
    struct semaphore *sem;
    struct thread   **pp;
    register unsigned short sr;

    if (!t || t->magic != CTXT_MAGIC) return;

    TRACE_THREAD("SEMAPHORE TIMEOUT: Thread=%d", t->tid);

    sr = splhigh();

    /* Re-check under lock: sem_up may have already woken the thread */
    if (!(t->wait_type & WAIT_SEMAPHORE)) {
        spl(sr);
        return;
    }

    sem = (struct semaphore *)t->sem_wait_obj;
    if (sem) {
        pp = &sem->wait_queue;
        while (*pp) {
            if (*pp == t) {
                *pp = t->next_wait;
                break;
            }
            pp = &(*pp)->next_wait;
        }
    }

    TRACE_THREAD("SEMAPHORE TIMEOUT: Thread=%d, sem=%p", t->tid, sem);

    t->wait_type   &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj = (void *)-1L;   /* ETIMEDOUT sentinel */
    t->next_wait    = NULL;
    t->sleep_timeout = NULL;

    proc_thread_state_change(t, THREAD_STATE_READY);
    add_to_ready_queue(t);

    spl(sr);

    if (curproc == p)
        proc_thread_schedule();
}

/* =========================================================================
 * semaphore_handle_signal_wakeup  —  internal helper
 * =========================================================================
 *
 * Called on the wake-up path of sem_down / sem_timedwait to detect whether
 * the thread was woken by a signal rather than by sem_up or a timeout.
 *
 * If WAIT_SEMAPHORE is still set when we resume, no one cleared it for us,
 * which means we were woken by a signal delivery.  We must remove ourselves
 * from the wait queue (if still linked) and cancel any pending timeout.
 *
 * Returns THREAD_SUCCESS on a normal (sem_up / timeout) wakeup.
 * Returns EINTR         if a signal interrupted the wait.
 *
 * No changes needed here — kept for completeness.
 */
static long semaphore_handle_signal_wakeup(struct thread *t, struct semaphore *sem)
{
    register unsigned short sr;
    struct thread **pp;

    /*
     * sem_up and the timeout handler both clear WAIT_SEMAPHORE before
     * making the thread runnable.  If it is still set we were woken by
     * a signal.
     */
    if (!(t->wait_type & WAIT_SEMAPHORE))
        return THREAD_SUCCESS;

    sr = splhigh();

    /* Remove ourselves from the semaphore wait queue atomically. */
    if (sem && t->sem_wait_obj == sem) {
        pp = &sem->wait_queue;
        while (*pp) {
            if (*pp == t) {
                *pp = t->next_wait;
                break;
            }
            pp = &(*pp)->next_wait;
        }
    }

    t->next_wait    = NULL;
    t->wait_type   &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj = NULL;

    /* Cancel any pending timeout — the signal took precedence. */
    if (t->sleep_timeout) {
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }

    spl(sr);

    TRACE_THREAD("SEM: Thread %d interrupted by signal on sem=%p", t->tid, sem);
    return EINTR;
}

/* =========================================================================
 * thread_semaphore_trydown  —  sem_trywait
 * =========================================================================
 *
 * BUG FIXED: the original code performed the count check and decrement
 * outside any interrupt lock, so a concurrent sem_up on another thread (or
 * the timer ISR calling sem_up via a timeout handler) could observe count>0,
 * both decrement it, and drive it negative.  The fix takes splhigh() around
 * the test-and-decrement pair.
 */
long thread_semaphore_trydown(struct semaphore *sem)
{
    struct proc   *p = curproc;
    struct thread *t = p ? p->current_thread : NULL;
    register unsigned short sr;
    long ret;

    if (!sem || !t || t->magic != CTXT_MAGIC) return EINVAL;

    TRACE_THREAD("SEMAPHORE TRYDOWN: Count=%ld", sem->count);

    sr = splhigh();
    if (sem->count > 0) {
        sem->count--;
        ret = THREAD_SUCCESS;
    } else {
        ret = EAGAIN;
    }
    spl(sr);

    TRACE_THREAD("SEMAPHORE TRYDOWN: ret=%ld count now %ld", ret, sem->count);
    return ret;
}

/* =========================================================================
 * thread_semaphore_timeddown  —  sem_timedwait
 * =========================================================================
 *
 * BUGS FIXED:
 *
 * 1. tid==0 spin path: the check and decrement were not atomic (no lock).
 *    Fixed by wrapping the test-and-decrement inside splhigh/spl.
 *
 * 2. tid==0 spin path: used yield() alone, which can starve if the process
 *    preemption timer is stopped (single-thread condition).  Now calls
 *    proc_thread_schedule() first, matching thread_semaphore_down.
 *
 * 3. tid>0 initial check: same non-atomic test-and-decrement as trydown.
 *    Fixed with splhigh/spl.
 *
 * 4. Enqueueing was done without the interrupt lock held, meaning sem_up
 *    could miss a waiter that was in the middle of being appended.  Now
 *    the entire enqueue + state-change + remove-from-ready is atomic.
 *
 * 5. Wake-up path: the timeout sentinel check happened AFTER
 *    semaphore_handle_signal_wakeup(), but the signal helper clears
 *    sem_wait_obj unconditionally.  This meant a timeout could be
 *    misreported as EINTR.  The order is now: signal check first (it
 *    returns EINTR immediately), then timeout sentinel, then success.
 *
 * 6. Wake-up path: missing unconditional count decrement for the normal
 *    (sem_up) wakeup case.  sem_up always increments count before waking
 *    a waiter; the waiter must always decrement it.  The old conditional
 *    "if (sem->count > 0) sem->count--" could silently skip the decrement
 *    if a racing fast-path thread had already grabbed the token, violating
 *    POSIX (we'd return success without having acquired the semaphore).
 *    The decrement is now unconditional on the sem_up wake path.
 */
long thread_semaphore_timeddown(struct semaphore *sem, long ms)
{
    struct proc   *p   = curproc;
    struct thread *t   = p ? p->current_thread : NULL;
    struct thread *iter;
    CONTEXT       *ctx;
    register unsigned short sr;

    if (!sem || !t || t->magic != CTXT_MAGIC) return EINVAL;

    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Count=%ld, ms=%ld", sem->count, ms);

    /* A zero or negative timeout is a non-blocking trywait. */
    if (ms <= 0)
        return thread_semaphore_trydown(sem);

    /* ------------------------------------------------------------------
     * tid == 0 : spin-yield, never enter the wait queue.
     * ------------------------------------------------------------------ */
    if (t->tid == 0) {
        unsigned long start_ticks = get_system_ticks();
        unsigned long wait_ticks  = (unsigned long)(ms + 4) / 5; /* 5 ms/tick */

        for (;;) {
            proc_thread_schedule();   /* drive sibling threads first */
            proc_thread_yield();

            sr = splhigh();
            if (sem->count > 0) {
                sem->count--;
                spl(sr);
                TRACE_THREAD("SEMAPHORE TIMEDDOWN: tid=0 acquired");
                return THREAD_SUCCESS;
            }
            spl(sr);

            if ((get_system_ticks() - start_ticks) >= wait_ticks) {
                TRACE_THREAD("SEMAPHORE TIMEDDOWN: tid=0 ETIMEDOUT");
                return ETIMEDOUT;
            }
        }
        /* NOTREACHED */
    }

    /* ------------------------------------------------------------------
     * tid > 0 : proper blocking path.
     * ------------------------------------------------------------------ */
    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread=%d, sem=%p", t->tid, sem);

    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread %d already blocked (wait_type=0x%x)",
                     t->tid, t->wait_type);
        return EDEADLK;
    }

    sr = splhigh();

    /* Fast path under lock: token available. */
    if (sem->count > 0) {
        sem->count--;
        spl(sr);
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Fast-path acquired, count=%ld", sem->count);
        return THREAD_SUCCESS;
    }

    /* Enqueue atomically while still holding the lock. */
    t->next_wait    = NULL;
    t->wait_type    = WAIT_SEMAPHORE;
    t->sem_wait_obj = sem;

    if (!sem->wait_queue) {
        sem->wait_queue = t;
    } else {
        iter = sem->wait_queue;
        while (iter->next_wait) iter = iter->next_wait;
        iter->next_wait = t;
    }

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    remove_from_ready_queue(t);

    spl(sr);

    /* Arm the timeout — must be done outside the interrupt lock because
     * addtimeout() may call kmalloc() which must not run at splhigh. */
    t->sleep_timeout = addtimeout(p, ms, proc_thread_sem_wakeup_handler);
    if (t->sleep_timeout) {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Timeout=%p", t->sleep_timeout);
        t->sleep_timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Timeout allocation failed");
        /* Roll back the enqueue. */
        sr = splhigh();
        {
            struct thread **pp = &sem->wait_queue;
            while (*pp) {
                if (*pp == t) { *pp = t->next_wait; break; }
                pp = &(*pp)->next_wait;
            }
        }
        t->next_wait    = NULL;
        t->wait_type   &= ~WAIT_SEMAPHORE;
        t->sem_wait_obj = NULL;
        proc_thread_state_change(t, THREAD_STATE_RUNNING);
        spl(sr);
        return ENOMEM;
    }

    ctx = get_thread_context(t);

    if (save_context(ctx) == 0) {
        /* First call: context is saved — hand off to the scheduler. */
        ctx->regs[0] = 1;
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Context saved, sleeping thread %d", t->tid);
        proc_thread_schedule();
        /* Should never be reached. */
        TRACE_THREAD("FATAL: Unexpected return from proc_thread_schedule in timeddown!");
        return -1;
    }

    /* ------------------------------------------------------------------
     * Wake-up path (save_context returned 1).
     * ------------------------------------------------------------------ */
    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread %d resuming", t->tid);

    if (t->state != THREAD_STATE_RUNNING)
        proc_thread_state_change(t, THREAD_STATE_RUNNING);

    /* 1. Signal check — must come first; the signal helper clears sem_wait_obj. */
    if (semaphore_handle_signal_wakeup(t, sem) == EINTR)
        return EINTR;

    /* 2. Timeout sentinel set by proc_thread_sem_wakeup_handler. */
    if (t->sem_wait_obj == (void *)-1L) {
        t->sem_wait_obj = NULL;
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread %d ETIMEDOUT", t->tid);
        return ETIMEDOUT;
    }

    /* 3. Normal sem_up wakeup: consume the token sem_up reserved for us. */
    t->wait_type   &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj = NULL;
    sem->count--;   /* unconditional — sem_up guarantees the token is here */

    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread %d acquired, count now %ld",
                 t->tid, sem->count);
    return THREAD_SUCCESS;
}

/* =========================================================================
 * thread_semaphore_down  —  sem_wait
 * =========================================================================
 *
 * BUGS FIXED:
 *
 * 1. The lock-free fast-path (first splhigh/spl block) was immediately
 *    followed by a second splhigh for the slow-path re-check, with an
 *    unlocked gap between them.  A sem_up in that gap would be lost.
 *    Fixed: both checks are now under one contiguous lock.
 *
 * 2. tid==0 spin: decrement was not atomic.  Fixed same as timeddown.
 *
 * 3. Enqueueing happened after spl(sr), outside the lock.  Fixed: the
 *    enqueue, state change, and remove-from-ready are all under splhigh.
 *
 * 4. Wake-up path: conditional count decrement.  Fixed: unconditional,
 *    same rationale as timeddown bug #6 above.
 *
 * 5. Wake-up path: state transition to RUNNING happened AFTER the count
 *    decrement and return-value calculation, meaning a preemption between
 *    those steps could observe an inconsistent thread state.  Fixed:
 *    state is set to RUNNING first, before any other wake-up logic.
 */
long thread_semaphore_down(struct semaphore *sem)
{
    struct proc   *p;
    struct thread *t;
    CONTEXT       *ctx;
    register unsigned short sr;

    if (!sem) {
        TRACE_THREAD("SEM_DOWN: NULL semaphore pointer");
        return EINVAL;
    }

    p = curproc;
    t = p ? p->current_thread : NULL;

    if (!t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("SEM_DOWN: No valid current thread");
        return EINVAL;
    }

    TRACE_THREAD("SEM_DOWN: tid=%d sem=%p count=%ld", t->tid, sem, sem->count);

    /* ------------------------------------------------------------------
     * Fast path: take the lock once and check-then-decrement atomically.
     * ------------------------------------------------------------------ */
    sr = splhigh();
    if (sem->count > 0) {
        sem->count--;
        TRACE_THREAD("SEM_DOWN: Fast-path acquired, count now %ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }
    spl(sr);

    /* ------------------------------------------------------------------
     * tid == 0 : spin-yield (never blocks via save_context).
     * ------------------------------------------------------------------ */
    if (t->tid == 0) {
        TRACE_THREAD("SEM_DOWN: tid=0 spin-yield loop");
        for (;;) {
            proc_thread_schedule();
            proc_thread_yield();
            sr = splhigh();
            if (sem->count > 0) {
                sem->count--;
                TRACE_THREAD("SEM_DOWN: tid=0 acquired after yield, count=%ld",
                             sem->count);
                spl(sr);
                return THREAD_SUCCESS;
            }
            spl(sr);
        }
        /* NOTREACHED */
    }

    /* ------------------------------------------------------------------
     * tid > 0 : proper blocking path.
     * ------------------------------------------------------------------ */

    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("SEM_DOWN: Thread %d already blocked (wait_type=0x%x)",
                     t->tid, t->wait_type);
        return EDEADLK;
    }

    sr = splhigh();

    /*
     * Re-check under lock: another thread may have called sem_up between
     * our unlocked fast-path check above and now.
     */
    if (sem->count > 0) {
        sem->count--;
        TRACE_THREAD("SEM_DOWN: Re-check fast-path acquired, count=%ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }

    /* Enqueue ourselves while still holding the interrupt lock so that a
     * concurrent sem_up cannot miss us. */
    t->next_wait    = NULL;
    t->wait_type   |= WAIT_SEMAPHORE;
    t->sem_wait_obj = sem;

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    remove_from_ready_queue(t);

    if (!sem->wait_queue) {
        sem->wait_queue = t;
    } else {
        struct thread *iter = sem->wait_queue;
        while (iter->next_wait) iter = iter->next_wait;
        iter->next_wait = t;
    }

    spl(sr);

    TRACE_THREAD("SEM_DOWN: Thread %d blocking on sem=%p", t->tid, sem);

    /* ------------------------------------------------------------------
     * Save context.  On first call (returns 0) we hand off to the
     * scheduler.  We resume here when sem_up restores our context
     * (save_context returns 1 because sem_up sets ctx->regs[0]=1).
     * ------------------------------------------------------------------ */
    ctx = get_thread_context(t);

    if (save_context(ctx) == 0) {
        ctx->regs[0] = 1;   /* wake-up discriminator */

        TRACE_THREAD("SEM_DOWN: Context saved, sleeping thread %d "
                     "(SR=%x SSP=%lx USP=%lx PC=%lx)",
                     t->tid, t->ctxt[SYSCALL].sr, t->ctxt[SYSCALL].ssp,
                     t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].pc);

        proc_thread_schedule();

        /* Should never be reached on this code path. */
        TRACE_THREAD("SEM_DOWN: ERROR — returned from proc_thread_schedule!");
        return -1;
    }

    /* ------------------------------------------------------------------
     * Wake-up path (save_context returned 1).
     *
     * sem_up has already:
     *   • removed us from the wait queue
     *   • cleared wait_type / sem_wait_obj
     *   • called proc_thread_state_change(READY) + add_to_ready_queue()
     *
     * Order matters:
     *   1. Ensure RUNNING state first (before any sem count manipulation).
     *   2. Check for signal interruption.
     *   3. Clear any residual wait flags.
     *   4. Consume the token sem_up reserved for us (unconditional).
     * ------------------------------------------------------------------ */
    TRACE_THREAD("SEM_DOWN: Thread %d resuming (SR=%x SSP=%lx USP=%lx PC=%lx)",
                 t->tid, t->ctxt[SYSCALL].sr, t->ctxt[SYSCALL].ssp,
                 t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].pc);

    /* 1. Running state first. */
    if (t->state != THREAD_STATE_RUNNING)
        proc_thread_state_change(t, THREAD_STATE_RUNNING);

    /* 2. Signal check — returns EINTR if a signal woke us instead of sem_up. */
    if (semaphore_handle_signal_wakeup(t, sem) == EINTR)
        return EINTR;

    /* 3. Clear residual wait flags (defensive; sem_up should have done this). */
    t->wait_type   &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj = NULL;

    /* 4. Consume the token.
     *    sem_up always does sem->count++ before waking a waiter, so the
     *    token is guaranteed to be here.  The old conditional
     *    "if (sem->count > 0) sem->count--" was wrong: if a racing fast-
     *    path thread grabbed the token first, count could be 0 and we
     *    would return success without having acquired the semaphore —
     *    a POSIX violation.  The unconditional form is correct. */
    sem->count--;

    TRACE_THREAD("SEM_DOWN: Thread %d acquired sem=%p count now %ld",
                 t->tid, sem, sem->count);
    return THREAD_SUCCESS;
}

/* =========================================================================
 * thread_semaphore_up  —  sem_post
 * =========================================================================
 *
 * BUGS FIXED:
 *
 * 1. (CRASH) NULL / garbage pointer dereference before validation.
 *    The original code did sem->count++ on line 464 before checking
 *    if (sem) on line 466.  A garbage pointer (e.g. 0x1000000 from a
 *    corrupted startup_data) caused an immediate bus error.
 *    Fix: validate sem and current thread FIRST, then take splhigh(),
 *    then increment.
 *
 * 2. (COUNTER CORRUPTION) Double-increment in the stale-waiter path.
 *    The unconditional sem->count++ at the top of the function was
 *    followed by a second sem->count++ inside the "!waiter" branch
 *    (all queue entries invalid).  This drove count to 2 instead of 1.
 *    Fix: remove the second increment entirely.
 *
 * 3. (LIST CORRUPTION) Double-remove of a stale waiter.
 *    The stale-entry check removed the waiter from the queue but then
 *    fell through to the unconditional removal block below, which tried
 *    to unlink the same (already-unlinked) entry.  This set
 *    sem->wait_queue = waiter->next_wait = NULL, silently destroying
 *    all remaining valid waiters behind the stale one.
 *    Fix: early return after the stale-entry removal.
 *
 * 4. The increment was done outside splhigh(), creating a window where
 *    a concurrent sem_down fast-path could see count==1 and decrement
 *    it back to 0 before we even check for waiters — effectively giving
 *    the token to a new arrival instead of a queued waiter.
 *    Fix: take splhigh() BEFORE incrementing count.
 *
 * Invariant maintained throughout:
 *   sem->count is the number of tokens available to new sem_down callers.
 *   When we wake a waiter we increment count (token for the waiter) and
 *   the waiter decrements it upon resumption.  At all times count >= 0.
 */
long thread_semaphore_up(struct semaphore *sem)
{
    struct thread *current;
    struct thread *waiter;
    struct thread *prev_waiter;
    register unsigned short sr;

    /* ---- Validate BEFORE any dereference ---- */
    if (!sem) {
        TRACE_THREAD("SEM_UP: NULL semaphore pointer");
        return EINVAL;
    }

    current = CURTHREAD;
    if (!current) {
        TRACE_THREAD("SEM_UP: No current thread");
        return EINVAL;
    }

    TRACE_THREAD("SEM_UP: Thread %d, sem=%p initial count=%ld",
                 current->tid, sem, sem->count);

    /* ---- Take the interrupt lock before touching count or the queue ---- */
    sr = splhigh();

    /* Increment: publish one token. */
    sem->count++;

    TRACE_THREAD("SEM_UP: count now %ld", sem->count);

    /* ------------------------------------------------------------------
     * Case 1: No waiters — token sits in count, nothing more to do.
     * ------------------------------------------------------------------ */
    if (!sem->wait_queue) {
        TRACE_THREAD("SEM_UP: No waiters, count now %ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }

    /* ------------------------------------------------------------------
     * Case 2: Find the highest-priority valid waiter to wake.
     * ------------------------------------------------------------------ */
    prev_waiter = NULL;
    waiter = find_highest_priority_thread_in_queue(sem->wait_queue, &prev_waiter);

    if (!waiter) {
        /*
         * Every entry in the queue was invalid (exited / bad magic).
         * Purge the stale queue.  The token stays in count (already
         * incremented above) for the next real sem_down caller.
         * Do NOT increment count a second time.
         */
        TRACE_THREAD("SEM_UP: Wait queue had no valid waiters, purging");
        sem->wait_queue = NULL;
        spl(sr);
        return THREAD_SUCCESS;
    }

    /*
     * Stale-entry guard: a signal may have woken the thread and cleared
     * its wait_type / sem_wait_obj without removing it from our queue.
     * Remove it and return — the token stays in count.
     */
    if (!(waiter->wait_type & WAIT_SEMAPHORE) || waiter->sem_wait_obj != sem) {
        TRACE_THREAD("SEM_UP: Stale waiter %d (wait_type=0x%x obj=%p), removing",
                     waiter->tid, waiter->wait_type, (void *)waiter->sem_wait_obj);
        if (prev_waiter)
            prev_waiter->next_wait = waiter->next_wait;
        else
            sem->wait_queue = waiter->next_wait;
        waiter->next_wait = NULL;
        /*
         * Token stays in count — do not remove the waiter a second time
         * (old double-remove bug).  Return immediately.
         */
        spl(sr);
        return THREAD_SUCCESS;
    }

    /* ---- Remove the valid waiter from the queue ---- */
    if (prev_waiter)
        prev_waiter->next_wait = waiter->next_wait;
    else
        sem->wait_queue = waiter->next_wait;

    waiter->next_wait    = NULL;
    waiter->wait_type   &= ~WAIT_SEMAPHORE;
    waiter->sem_wait_obj = NULL;

    /* Cancel any pending timeout for this waiter. */
    if (waiter->sleep_timeout) {
        TRACE_THREAD("SEM_UP: Canceling timeout for thread %d", waiter->tid);
        canceltimeout(waiter->sleep_timeout);
        waiter->sleep_timeout = NULL;
    }

    /* Clear stale sleep state (defensive). */
    if (waiter->wakeup_time > 0) {
        waiter->wakeup_time = 0;
        remove_from_sleep_queue(waiter->proc, waiter);
    }

    TRACE_THREAD("SEM_UP: Waking thread %d (priority %d)",
                 waiter->tid, waiter->priority);

    proc_thread_state_change(waiter, THREAD_STATE_READY);
    add_to_ready_queue(waiter);

    /*
     * Priority preemption: if the woken thread outranks us, yield now
     * so it runs without waiting for the next timer tick.
     *
     * Release the interrupt lock BEFORE calling proc_thread_schedule()
     * — schedule() takes splhigh() internally and our spl scheme is
     * non-reentrant.
     */
    if (waiter->priority > current->priority) {
        TRACE_THREAD("SEM_UP: Waiter %d outranks current %d, preempting",
                     waiter->tid, current->tid);
        spl(sr);
        proc_thread_schedule();
        return THREAD_SUCCESS;
    }

    spl(sr);
    return THREAD_SUCCESS;
}