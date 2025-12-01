/**
 * @file proc_threads_sleep_yield.c
 * @brief Kernel Thread Timing Operations
 * 
 * Implements sleep, yield, and timeout management in the kernel scheduler.
 * 
 * Features:
 *  - Precision sleep timing
 *  - Yield operations
 *  - Sleep queue management
 *  - Priority boosting for time-sensitive threads
 *  - Wakeup optimization
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#include "proc_threads_sleep_yield.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_cancel.h"
#include "proc_threads_signal.h"


/* Maximum value for an unsigned long */
#ifndef ULONG_MAX
#define ULONG_MAX 0xFFFFFFFFUL
#endif

/**
 * Wake threads that have reached their wakeup time
 * 
 * @param p Process containing threads to check
 * @param current_time Current system time in ticks
 * @return Number of threads woken
 */
/**
 * Bitmap-optimized sleep queue processing
 * Replaces wake_threads_by_time() with better performance
 */
int wake_threads_by_time(struct proc *p, unsigned long current_time) {
    if (!p || !p->sleep_queue) {
        return 0;
    }
    
    unsigned short wakeable_bitmap = 0;
    int total_wakeable = 0;
    struct thread *t = NULL;

    // First pass: build bitmap of wakeable priorities
    for (t = p->sleep_queue; t; t = t->next_sleeping) {
        if (t->magic == CTXT_MAGIC &&
            (t->state & THREAD_STATE_BLOCKED) && !(t->state & THREAD_STATE_EXITED) &&
            ( (t->wakeup_time > 0 && t->wakeup_time <= current_time) || (t->t_sigpending & ~THREAD_SIGMASK(t)) )
        
        ) {
            
            wakeable_bitmap |= (1 << t->priority);
            total_wakeable++;
        }
    }
    
    if (!wakeable_bitmap) {
        return 0;
    }
    
    TRACE_THREAD("SLEEP: Wakeable bitmap: 0x%04x (%d threads)", wakeable_bitmap, total_wakeable);
    
    // Wake threads in priority order (highest first)
    int woken = 0;
    while (wakeable_bitmap && woken < total_wakeable) {
        int highest_pri = find_highest_priority_bit_word(wakeable_bitmap);
        wakeable_bitmap &= ~(1 << highest_pri); // Clear this priority
        
        // Wake all threads at this priority level
        struct thread **tp = &p->sleep_queue;
        while (*tp && woken < total_wakeable) {
            t = *tp;
            // Check for cancellation before waking up
            if (t->cancel_pending && t->cancel_state == PTHREAD_CANCEL_ENABLE) {
                check_thread_cancellation(t);
            }
            if (t->magic == CTXT_MAGIC &&
                (t->state & THREAD_STATE_BLOCKED) &&
                !(t->state & THREAD_STATE_EXITED) &&
                t->priority == highest_pri &&
                ( (t->wakeup_time > 0 && t->wakeup_time <= current_time) || (t->t_sigpending & ~THREAD_SIGMASK(t)) )
            ) {
                
                TRACE_THREAD("SLEEP_CHECK: Thread %d should wake up!", t->tid);
                
                // Remove from sleep queue (inline removal for efficiency)
                *tp = t->next_sleeping;
                t->next_sleeping = NULL;
                
                // Apply priority boost logic
                boost_thread_priority(t, 5); // Boost by 5 levels
                
                // Clear sleep state
                t->wait_type &= ~WAIT_SLEEP;
                t->wakeup_time = 0;
                
                // Update thread state and add to ready queue
                atomic_thread_state_change(t, THREAD_STATE_READY);
                add_to_ready_queue(t);
                woken++;
                
                TRACE_THREAD("SLEEP: Woke thread %d (pri %d)", t->tid, t->priority);
                
                // Don't advance tp since we removed current element
            } else {
                tp = &t->next_sleeping;
            }
        }
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

    #ifdef DEBUG_THREAD
    unsigned long current_time = get_system_ticks();
    
    TRACE_THREAD("SLEEP_CHECK: Checking sleep queue for process %d at time %lu", 
                p->pid, current_time);
    
    // Debug: dump sleep queue
    struct thread *debug_t = p->sleep_queue;
    while (debug_t) {
        TRACE_THREAD("  Thread %d (wake at %lu, current %lu, state=%d)", 
                    debug_t->tid, 
                    debug_t->wakeup_time, 
                    current_time,
                    debug_t->state);
        debug_t = debug_t->next_sleeping;
    }
    #endif
    
    // Wake threads that have reached their wakeup time
    wake_threads_by_time(p, get_system_ticks());
}

/**
 * @brief Thread sleep wakeup handler
 * 
 * This function is called when a thread's sleep timer expires. It checks
 * the validity of the thread and ensures it is actually sleeping before
 * waking it up. If the thread was cancelled during its sleep, it will
 * be terminated instead of waking up.
 *
 * @param p Process containing the thread to wake up
 * @param arg Pointer to the thread to wake up
 */
void proc_thread_sleep_wakeup_handler(PROC *p, long arg) {
    struct thread *t = (struct thread *)arg;
    
    if (!t || !p || t->proc != p) {
        return;
    }

    // Critical safety check: Don't wake up exited threads
    if (t->magic != CTXT_MAGIC || (t->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("SLEEP_WAKEUP: Thread %d is invalid or exited (magic=%lx, state=%d), ignoring wakeup", 
                    t->tid, t->magic, t->state);
        return;
    }
    
    // Additional check: ensure thread is actually sleeping
    if (!(t->state & THREAD_STATE_BLOCKED) || !(t->wait_type & WAIT_SLEEP)) {
        TRACE_THREAD("SLEEP_WAKEUP: Thread %d is not sleeping (state=%d, wait_type=%d), ignoring wakeup", 
                    t->tid, t->state, t->wait_type);
        return;
    }
    
    TRACE_THREAD("SLEEP_WAKEUP: Valid wakeup for thread %d", t->tid);
    

    // Check for cancellation before waking up
    if (t->cancel_pending && t->cancel_state == PTHREAD_CANCEL_ENABLE) {
        check_thread_cancellation(t);
        return;
    }

    // Thread is valid and sleeping, proceed with wakeup
    TRACE_THREAD("SLEEP_WAKEUP: Direct wakeup for thread %d", t->tid);

    // Boost priority
    boost_thread_priority(t, 5);  // Boost by 5 levels
    
    // Wake up thread
    t->wait_type &= ~WAIT_SLEEP;
    t->wakeup_time = 0;  // Clear wake-up time
    t->sleep_timeout = NULL;  // Clear timeout reference
    remove_from_sleep_queue(p, t);
    atomic_thread_state_change(t, THREAD_STATE_READY);
    add_to_ready_queue(t);
    
    // Force a schedule to run this thread immediately if possible

    reschedule_preemption_timer(p, t->tid);
    proc_thread_schedule();
}

/**
 * Clean up sleep state when a thread is being destroyed
 */
void cleanup_thread_sleep_state(struct thread *t) {
    if (!t) return;
    
    // Cancel sleep timeout if active
    if (t->sleep_timeout) {
        TRACE_THREAD("CLEANUP: Cancelling sleep timeout for thread %d", t->tid);
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }
    
    // Clear sleep state
    if (t->wait_type & WAIT_SLEEP) {
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0;
        
        // Remove from sleep queue if present
        if (t->proc && t->proc->sleep_queue) {
            remove_from_sleep_queue(t->proc, t);
        }
    }
    
    return;
}

long proc_thread_sleep(long ms) {
    struct proc *p = curproc;
    struct thread *t = p ? p->current_thread : NULL;
    
    TRACE_THREAD("SLEEP: Thread %d sleeping for %d ms", t->tid, ms);
    if (!p || !t) return EINVAL;
    if (t->tid == 0) return EINVAL; // thread0 can't sleep
    if (ms <= 0) return 0; // No need to sleep

    pthread_testcancel_internal(t);

    // Prevent nested blocking
    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("SLEEP: Thread %d already blocked", t->tid);
        return EDEADLK;
    }

    unsigned long current_time = get_system_ticks();
    unsigned long ticks;
    long ret = 0;
    // Handle special cases
    if (ms <= 0) {
        // No sleep or invalid sleep time
        ticks = 0;
    } else if (ms >= ULONG_MAX / 2) {
        // Very long sleep time, cap it to avoid overflow
        ticks = ULONG_MAX / 2;
        TRACE_THREAD("SLEEP: Sleep time too large, capping to %lu ticks", ticks);
    } else {
        // Normal case: convert ms to ticks with rounding up
        ticks = (ms + MS_PER_TICK - 1) / MS_PER_TICK;
    }

    // Set wakeup time
    t->wakeup_time = current_time + ticks;

    TRACE_THREAD_SLEEP(t, ms, ticks, t->wakeup_time);
    
    // Remove from sleep queue if already there
    remove_from_sleep_queue(p, t);
    
    // Add to sleep queue
    t->next_sleeping = p->sleep_queue;
    p->sleep_queue = t;
    TRACE_THREAD("SLEEP: Added thread %d to sleep queue (wake at %lu)", 
                t->tid, t->wakeup_time);
    
        
    // Set up a direct timeout for this thread
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

        // First time through - this is the "going to sleep" path
        // memcpy(&t->ctxt[SYSCALL], &t->proc->ctxt[SYSCALL], sizeof(CONTEXT));
        TRACE_THREAD("SLEEP: SAVED SYSCALL CONTEXT: Thread %d context - SSP=%lx, USP=%lx, PC=%lx", t->tid, t->ctxt[SYSCALL].ssp, t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].pc);        
        TRACE_THREAD_SLEEP(t, ms, ticks, t->wakeup_time);
        
        atomic_thread_state_change(t, THREAD_STATE_BLOCKED);
        t->wait_type |= WAIT_SLEEP;  // Set wait type
        remove_from_ready_queue(t);
        t->cpu_time += (get_system_ticks() - t->last_scheduled); // Update CPU time before sleeping
        // Schedule another thread
        proc_thread_schedule();
        
        // We should never reach here - proc_thread_schedule() will switch to another thread
        TRACE_THREAD("SLEEP: ERROR - Returned from proc_thread_schedule() in sleep path!");
        return -1;
    } else {
        // Second time through - this is the "waking up" path
        // This code runs when the thread is awakened and context is restored

        pthread_testcancel_internal(t);

        // Check for pending signals before going to sleep
        if (t->t_sigpending) {
            ulong pending_unmasked = t->t_sigpending & ~THREAD_SIGMASK(t);
            if (pending_unmasked) {
                // t->cpu_time += 1; // Increment CPU time to avoid being "too new"
                TRACE_THREAD("SLEEP: Thread %d has pending signals 0x%lx, not sleeping (cpu_time=%lu, last_scheduled=%lu)", t->tid, pending_unmasked, t->cpu_time, t->last_scheduled);
                dispatch_thread_signals(t);
            }
        } else {
            TRACE_THREAD("SLEEP: Thread %d has no pending signals, proceeding to wakeup", t->tid);
        }
        // When we return, check if we woke up on time
        current_time = get_system_ticks();
        if (t->wakeup_time > 0 && current_time > t->wakeup_time) {
            TRACE_THREAD("SLEEP: Thread %d woke up late by %lu ms",
                        t->tid, (current_time - t->wakeup_time) * MS_PER_TICK);
        }
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0; // Clear wake-up time
        
        // Ensure we're in the RUNNING state
        if (t->state != THREAD_STATE_RUNNING) {
            TRACE_THREAD("SLEEP: Thread %d not in RUNNING state after wake, fixing", t->tid);
            atomic_thread_state_change(t, THREAD_STATE_RUNNING);
        }

        TRACE_THREAD_WAKEUP(t);
        return ret;
    }
}

long proc_thread_yield(void) {
    struct proc *p = curproc;
    struct thread *t;
    
    if (!p || !p->current_thread){
        TRACE_THREAD("YIELD: No current thread");
        yield();
        return 0;
    }
    if(p->current_thread->tid == 0) {
        TRACE_THREAD("YIELD: Thread %d yielded, rescheduling", p->current_thread->tid);
        // if(p->num_threads <= 1) 
        yield();
        // else proc_thread_schedule();
        return 0;
    }
    if(p->current_thread->is_idle) {
        // TRACE_THREAD("YIELD: Idle thread %d yielded, rescheduling", p->current_thread->tid);
        yield();
        return 0;
    }
                
    t = p->current_thread;
    
    // Check if yield is beneficial
    unsigned long now = get_system_ticks();
    unsigned long elapsed = now - t->last_scheduled;
    
    TRACE_THREAD("YIELD: Thread %d - now=%lu, last_scheduled=%lu, elapsed=%lu", 
                t->tid, now, t->last_scheduled, elapsed);
    
    // Don't yield if no other threads at same/higher priority
    struct thread *next = get_highest_priority_thread(p);
    if (!next || next == t) {
        TRACE_THREAD("YIELD: Thread %d - no other threads to yield to", t->tid);
        return 0;
    }
    
    // Prevent excessive yielding (anti-livelock protection)
    if (elapsed < 2) {
        TRACE_THREAD("YIELD: Thread %d yielding too frequently (%lu ticks), ignoring", 
                    t->tid, elapsed);
        return 0;
    }
    
    // TRACE_THREAD("YIELD: Thread %d yielding after %lu ticks to thread %d", 
    //             t->tid, elapsed, next->tid);
    
    // For SCHED_FIFO and SCHED_RR, move to end of same-priority list
    if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
        
        // Only if we're in RUNNING state
        if (t->state == THREAD_STATE_RUNNING) {
            // Change to READY and add to end of ready queue
            atomic_thread_state_change(t, THREAD_STATE_READY);
            
            // Add to ready queue (will be added at end of same-priority list)
            add_to_ready_queue(t);
            
            // Force a reschedule
            proc_thread_schedule();
            return 0;
        }
        
    } else {
        // For SCHED_OTHER, just call schedule
        proc_thread_schedule();
    }
    
    return 0;
}
