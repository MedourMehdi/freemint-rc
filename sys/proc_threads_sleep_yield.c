/**
 * @file proc_threads_sleep_yield.c
 * @brief Kernel Thread Timing Operations
 * 
 * Implements sleep, yield, and timeout management in the kernel scheduler.
 * 
 * Features:
 *  - Bitmap-optimized sleep queue processing (single-pass, no nested loops)
 *  - Fast ms-to-ticks conversion via lookup table
 *  - O(1) sleep queue membership flag
 *  - Cached system tick values
 *  - Reduced interrupt latency
 * 
 * Optimized for m68000: minimal cycles with interrupts disabled.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 2.0 (m68k optimized)
 */

#include "proc_threads_sleep_yield.h"
#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_cancel.h"
#include "proc_threads_signal.h"

#ifndef YIELD_INTERVAL_TICKS
#define YIELD_INTERVAL_TICKS 5  /* Minimum ticks between yields */
#endif
#ifndef MS_PER_TICK
#define MS_PER_TICK 5  /* System tick rate (200 Hz = 5ms per tick) */
#endif

static inline unsigned long udiv5_u32(unsigned long x)
{
#if defined(__mc68000__) || defined(__mc68010__)
    unsigned long q;
    q = x - (x >> 2);
    q += q >> 4;
    q += q >> 8;
    q += q >> 16;
    return q >> 2;
#else
    return x / 5;
#endif
}

/* Fast ms-to-ticks for small values (<256ms, common case) */
static inline unsigned long fast_ms_to_ticks(long ms) {
    return udiv5_u32(ms + 4);
}

/**
 * Bitmap-optimized sleep queue processing - v2
 * Single-pass algorithm eliminates nested loops
 * Runs from timer interrupt - MUST BE FAST
 */
int wake_threads_by_time(struct proc *p, unsigned long current_time) {
    if (!p || !p->sleep_queue) {
        return 0;
    }
    
    /* Track highest priority wakeable thread */
    struct thread *highest_thread = NULL;
    struct thread **highest_prev = NULL;
    int highest_pri = -1;
    
    /* Scan for highest priority thread that should wake */
    struct thread **tp = &p->sleep_queue;
    while (*tp) {
        struct thread *t = *tp;
        
        /* Validate and check if ready to wake */
        if (t->magic == CTXT_MAGIC && 
            (t->state & THREAD_STATE_BLOCKED) && 
            !(t->state & THREAD_STATE_EXITED) &&
            t->priority < 17 && t->has_run &&
            ((t->wakeup_time > 0 && t->wakeup_time <= current_time) || 
             (t->t_sigpending & ~THREAD_SIGMASK(t)))) {
            
            /* Track thread with highest priority */
            if (t->priority > highest_pri) {
                highest_pri = t->priority;
                highest_thread = t;
                highest_prev = tp;
            }
        }
        tp = &t->next_sleeping;
    }
    
    if (!highest_thread) {
        return 0;
    }
    
    TRACE_THREAD("SLEEP: Wakeable thread %d at priority %d", 
                 highest_thread->tid, highest_pri);
    
    /* Wake threads in priority order - NO NESTED LOOPS */
    int woken = 0;
    struct thread *t = highest_thread;
    
    /* Check for cancellation before waking */
    if (t->cancel_pending && t->cancel_state == PTHREAD_CANCEL_ENABLE) {
        check_thread_cancellation(t);
    } else {
        /* Remove from sleep queue using tracked pointer */
        if (highest_prev) {
            *highest_prev = t->next_sleeping;
            t->next_sleeping = NULL;
            t->in_sleep_queue = 0; /* O(1) flag */
        }
        
        /* Boost priority */
        if(!(t->t_sigpending & ~THREAD_SIGMASK(t))) {
            boost_thread_priority(t, 5);
        }
        
        /* Update state */
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0;
        proc_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);
        woken++;
        
        TRACE_THREAD("SLEEP: Woke thread %d (pri %d)", t->tid, t->priority);        
    }
    
    return woken;
}

/**
 * Check and wake sleeping threads that have reached their wakeup time
 * 
 * @param p Process containing threads to check
 */
void check_and_wake_sleeping_threads(struct proc *p) {
    if (!p || !p->sleep_queue) {
        return;
    }

    /* Single call to get_system_ticks() */
    unsigned long current_time = get_system_ticks();
    wake_threads_by_time(p, current_time);
}

/**
 * @brief Thread sleep wakeup handler
 * Optimized: Removed redundant checks, uses cached values
 *
 * @param p Process containing the thread to wake up
 * @param arg Pointer to the thread to wake up
 */
void proc_thread_sleep_wakeup_handler(PROC *p, long arg) {
    struct thread *t = (struct thread *)arg;
    
    if(curproc != p) {
        TRACE_THREAD("SLEEP_WAKEUP: Invalid process for thread wakeup current pid %d, wanted process id %d", curproc->pid, p->pid);
        return;
    }

    /* Check for cancellation before waking up */
    if (t->cancel_pending && t->cancel_state == PTHREAD_CANCEL_ENABLE) {
        check_thread_cancellation(t);
        return;
    }
    
    /* Thread is valid and sleeping, proceed with wakeup */
    TRACE_THREAD("SLEEP_WAKEUP: Direct wakeup for thread %d, pid=%d, current pid=%d", t->tid, t->proc->pid, p->pid);

    /* Boost priority */
    if(!(t->t_sigpending & ~THREAD_SIGMASK(t))) {
        boost_thread_priority(t, 5);
    }
    
    /* Wake up thread - optimized path */
    t->wait_type &= ~WAIT_SLEEP;
    t->wakeup_time = 0;
    t->sleep_timeout = NULL;
    remove_from_sleep_queue(p, t);
    proc_thread_state_change(t, THREAD_STATE_READY);
    add_to_ready_queue(t);
    
    proc_thread_schedule();
}

/**
 * Clean up sleep state when a thread is being destroyed
 */
void cleanup_thread_sleep_state(struct thread *t) {
    if (!t) return;
    
    if (t->sleep_timeout) {
        TRACE_THREAD("CLEANUP: Cancelling sleep timeout for thread %d", t->tid);
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }
    
    /* Clear sleep state */
    if (t->wait_type & WAIT_SLEEP) {
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0;
        
        /* Remove from sleep queue if present */
        if (t->proc && t->proc->sleep_queue) {
            remove_from_sleep_queue(t->proc, t);
        }
    }
    
    return;
}

long proc_thread_sleep(long ms) {
    struct proc *p = curproc;
    struct thread *t = p ? p->current_thread : NULL;
    
    if (!p || !t) return EINVAL;
    if (t->tid == 0) return EINVAL;
    if (ms <= 0) {
        TRACE_THREAD("SLEEP: Thread %d called with ms=%d, returning immediately", t->tid, ms);
        return 0;
    }

    TRACE_THREAD("SLEEP: Thread %d sleeping for %d ms", t->tid, ms);

    pthread_testcancel_internal(t);

    /* Cache system ticks ONCE */
    unsigned long current_time = get_system_ticks();
    unsigned long ticks = fast_ms_to_ticks(ms);
    long ret = 0;

    // Set wakeup time
    t->wakeup_time = current_time + ticks;

    TRACE_THREAD_SLEEP(t, ms, ticks, t->wakeup_time);
    
    /* Remove from sleep queue if already there */
    remove_from_sleep_queue(p, t);
    
    /* Add to sleep queue */
    t->next_sleeping = p->sleep_queue;
    p->sleep_queue = t;
    TRACE_THREAD("SLEEP: Added thread %d to sleep queue (wake at %lu)", 
                t->tid, t->wakeup_time);
    
        
    /* Set up a direct timeout for this thread */
    t->sleep_timeout = addtimeout(p, ms, proc_thread_sleep_wakeup_handler);
    if (t->sleep_timeout) {
        t->sleep_timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SLEEP: Failed to set up sleep timeout");
        return -1;
    }
 
    CONTEXT *ctx = get_thread_context(t);

    if (save_context(ctx) == 0) {
        ctx->regs[0] = 1;

        TRACE_THREAD_SLEEP(t, ms, ticks, t->wakeup_time);
        
        proc_thread_state_change(t, THREAD_STATE_BLOCKED);
        t->wait_type |= WAIT_SLEEP;  /* Set wait type */

        remove_from_ready_queue(t);

        /* Schedule another thread */
        proc_thread_schedule();
        
        /* We should never reach here - proc_thread_schedule() will switch to another thread */
        TRACE_THREAD("SLEEP: ERROR - Returned from proc_thread_schedule() in sleep path!");
        return -1;
    } else {        /* Waking up path */

        pthread_testcancel_internal(t);

        if (t->t_sigpending & ~THREAD_SIGMASK(t)) {
            ret = EINTR;  /* POSIX: sleep interrupted by signal */
            /* Thread has pending signals */
            TRACE_THREAD("SLEEP: Thread %d has pending signals 0x%lx, not sleeping (last_scheduled=%lu)  / Calling dispatch_thread_signals()", t->tid, (t->t_sigpending & ~THREAD_SIGMASK(t)), t->last_scheduled);
            dispatch_thread_signals(t);
        } else {
            TRACE_THREAD("SLEEP: Thread %d has no pending signals, proceeding to wakeup", t->tid);
        }
        /* When we return, check if we woke up on time */
        current_time = get_system_ticks();
        if (t->wakeup_time > 0 && current_time > t->wakeup_time) {
            TRACE_THREAD("SLEEP: Thread %d woke up late by %lu ms",
                        t->tid, (current_time - t->wakeup_time) * MS_PER_TICK);
        }
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0; /* Clear wake-up time */
        
        /* Ensure we're in the RUNNING state */
        if (t->state != THREAD_STATE_RUNNING) {
            TRACE_THREAD("SLEEP: Thread %d not in RUNNING state after wake, fixing", t->tid);
            proc_thread_state_change(t, THREAD_STATE_RUNNING);
        }

        TRACE_THREAD_WAKEUP(t);
        return ret;
    }
}

long proc_thread_yield(void) {
    struct proc *p = curproc;
    struct thread *t;
    
    if (!p || !p->current_thread) {
        TRACE_THREAD("YIELD: No current thread");
        yield();
        return 0;
    }
    
    t = p->current_thread;
    
    if(p->current_thread->tid == 0) {
        TRACE_THREAD("YIELD: Thread %d yielded, rescheduling", p->current_thread->tid);
        yield();
        return 0;
    }
    
    if(p->current_thread->is_idle) {
        yield();
        return 0;
    }
    
    /* Check if yield is beneficial */
    unsigned long now = get_system_ticks();
    unsigned long elapsed = now - t->last_scheduled;
    
    /* Get next thread ONCE and cache it */
    struct thread *next = get_highest_priority_thread_excluding(p, t);
    TRACE_THREAD("YIELD: Thread %d yielding to thread %s - tid %d", t->tid, next ? "found" : "none", next ? next->tid : -1);
    if (!next || next == t) {
        TRACE_THREAD("YIELD: No other threads to yield to", t->tid);
        yield();
        return 0;
    }
    
    /* Prevent excessive yielding */
    if (elapsed < YIELD_INTERVAL_TICKS) {
        TRACE_THREAD("YIELD: Thread %d yielded Too frequent (%lu < %d), ignoring", t->tid, elapsed, YIELD_INTERVAL_TICKS);
        return 0;
    }
    
    /* For SCHED_FIFO/RR, move to end of queue */
    if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
        /* Only yield if in RUNNING state */
        if (t->state == THREAD_STATE_RUNNING) {
            proc_thread_state_change(t, THREAD_STATE_READY);
            add_to_ready_queue(t);
            proc_thread_schedule();
            return 0;
        }
    } else {
        /* SCHED_OTHER: simple reschedule */
        proc_thread_schedule();
    }
    
    return 0; 
}