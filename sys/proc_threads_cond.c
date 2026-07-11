
/**
 * @file proc_threads_cond.c
 * @brief Kernel-level Thread Synchronization
 * 
 * Implements core synchronization primitives (mutexes, condition variables,
 * semaphores) and thread lifecycle operations within the FreeMiNT kernel.
 * Handles join/detach operations and process termination cleanup.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads_cond.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"

/* Timeout handler */
static void proc_thread_condvar_timeout_handler(PROC *p, long arg);

/**
 * magic validation with destruction check
 */
static inline int validate_condvar(struct condvar *cond) {
    return (cond && cond->magic == CONDVAR_MAGIC && !cond->destroyed);
}

/**
 * Atomic mutex association to prevent race conditions
 */
static int associate_mutex_atomic(struct condvar *cond, struct mutex *mutex) {
    register unsigned short sr = splhigh();
    
    if (!cond->associated_mutex) {
        cond->associated_mutex = mutex;
        spl(sr);
        return 0;
    } else if (cond->associated_mutex != mutex) {
        spl(sr);
        return EINVAL;
    }
    spl(sr);
    return 0;
}

/**
 * Find first waiting thread (FIFO order) for POSIX compliance
 */
static struct thread *find_first_waiting_thread(struct thread *wait_queue, struct thread **prev_thread) {
    *prev_thread = NULL;
    return wait_queue;  // First thread in queue (FIFO)
}

/**
 * Remove thread from wait queue atomically
 */
static void remove_thread_from_condvar_queue(struct condvar *cond, struct thread *thread_to_remove, struct thread *prev_thread) {
    TRACE_THREAD("CONDVAR WAKE: Removing thread %d from condvar wait queue", thread_to_remove->tid);
    if (prev_thread) {
        prev_thread->next_wait = thread_to_remove->next_wait;
    } else {
        cond->wait_queue = thread_to_remove->next_wait;
    }
    TRACE_THREAD("CONDVAR WAKE: Thread %d removed from condvar wait queue", thread_to_remove->tid);
    thread_to_remove->next_wait = NULL;
}

/**
 * Wake up a thread from condition variable wait
 */
static void wakeup_condvar_thread(struct thread *thread) {
    TRACE_THREAD("CONDVAR WAKE: Waking up thread %d whatever it was waiting for", thread->tid);
    // Clear wait state
    thread->wait_type &= ~WAIT_CONDVAR;
    thread->cond_wait_obj = NULL;
    
    // Remove from sleep queue if needed (for timedwait)
    if (thread->wakeup_time > 0) {
        remove_from_sleep_queue(thread->proc, thread);
        thread->wakeup_time = 0;
    }
    
    // Mark as ready and add to ready queue
    proc_thread_state_change(thread, THREAD_STATE_READY);
    add_to_ready_queue(thread);
}

/**
 * Initialize a condition variable
 */
int proc_thread_condvar_init(struct condvar *cond) {
    if (!CURTHREAD) {
        TRACE_THREAD("CONDVAR INIT: No current thread");
        if(!handle_thread_mode_switching(curproc)) return EINVAL;
    }
    if (!cond) {
        return EINVAL;
    }
    
    cond->wait_queue = NULL;
    cond->associated_mutex = NULL;
    cond->magic = CONDVAR_MAGIC;
    cond->destroyed = 0;
    cond->timeout_ms = 0;

    TRACE_THREAD("CONDVAR INIT: condvar=%p", cond);
    return THREAD_SUCCESS;
}

/**
 * Destroy a condition variable
 */
int proc_thread_condvar_destroy(struct condvar *cond) {
    if (!validate_condvar(cond)) {
        return EINVAL;
    }
    
    register unsigned short sr = splhigh();
    
    // Check if any threads are waiting
    if (cond->wait_queue) {
        spl(sr);
        TRACE_THREAD("CONDVAR DESTROY: threads still waiting on condvar=%p", cond);
        return EBUSY;
    }
    
    cond->magic = 0;
    cond->destroyed = 1;

    spl(sr);
    TRACE_THREAD("CONDVAR DESTROY: condvar=%p destroyed", cond);
    return THREAD_SUCCESS;
}

/**
 * Wait on a condition variable
 */
int proc_thread_condvar_wait(struct condvar *cond, struct mutex *mutex) {
    if (!validate_condvar(cond) || !mutex) {
        TRACE_THREAD("CONDVAR WAIT: Invalid condvar or mutex");
        return EINVAL;
    }

    struct thread *t = CURTHREAD;
    if (!t) {
        TRACE_THREAD("CONDVAR WAIT: No current thread");
        return EINVAL;
    }

    // Prevent nested blocking
    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("CONDVAR WAIT: Thread %d already blocked", t->tid);
        return EDEADLK;
    }

    // Verify thread owns the mutex
    if (mutex->owner != t) {
        TRACE_THREAD("CONDVAR WAIT: Thread %d doesn't own mutex %p, owned by thread pointer %d", t->tid, mutex, mutex->owner);
        return EINVAL;
    }

    register unsigned short sr;
    
    TRACE_THREAD("CONDVAR WAIT: Thread %d preparing to wait on condvar=%p with mutex=%p", t->tid, cond, mutex);

    // Atomic mutex association
    int assoc_result = associate_mutex_atomic(cond, mutex);

    TRACE_THREAD("CONDVAR WAIT: Mutex %p associated with condvar %p result=%d", mutex, cond, assoc_result);

    if (assoc_result != 0) {
        TRACE_THREAD("CONDVAR WAIT: Failed to associate mutex %p with condvar %p", mutex, cond);
        return EINVAL;
    }
    
    TRACE_THREAD("CONDVAR WAIT: Thread %d waiting on condvar=%p", t->tid, cond);
    
    // Add to condition variable wait queue
    t->wait_type |= WAIT_CONDVAR;
    t->cond_wait_obj = cond;

    sr = splhigh();

    // Add to wait queue (FIFO order for condition variables)
    struct thread **tqueue = &cond->wait_queue;
    while (*tqueue) tqueue = &(*tqueue)->next_wait;
    *tqueue = t;
    t->next_wait = NULL;
    
    spl(sr);

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    
    // Release the mutex before blocking
    int unlock_result = thread_mutex_unlock(mutex);
    if (unlock_result != THREAD_SUCCESS) {
        // Remove from wait queue if unlock failed
        TRACE_THREAD("CONDVAR WAIT: Failed to unlock mutex %p for thread %d", mutex, t->tid);
        sr = splhigh();

        struct thread **pp = &cond->wait_queue;
        while (*pp && *pp != t) pp = &(*pp)->next_wait;
        if (*pp) *pp = t->next_wait;
        
        t->wait_type &= ~WAIT_CONDVAR;
        t->cond_wait_obj = NULL;
        t->next_wait = NULL;

        spl(sr);

        proc_thread_state_change(t, THREAD_STATE_READY);
        TRACE_THREAD("CONDVAR WAIT: Thread %d failed to unlock mutex %p, removed from condvar wait queue", t->tid, mutex);
        return unlock_result;
    }
    TRACE_THREAD("CONDVAR WAIT: Thread %d released mutex %p and is now blocked on condvar=%p", t->tid, mutex, cond);
    // Block until signaled
    proc_thread_schedule();
    
    // When we wake up, reacquire the mutex
    int lock_result = thread_mutex_lock(mutex);
    TRACE_THREAD("CONDVAR WAIT: Thread %d reacquired mutex %p after waiting on condvar=%pn result=%d", t->tid, mutex, cond, lock_result);
    // Clear wait state (should already be cleared by signal/broadcast)
    sr = splhigh();
    if (t->wait_type & WAIT_CONDVAR) {
        TRACE_THREAD("CONDVAR WAIT: Thread %d woke up but still waiting on condvar=%p", t->tid, cond);
        t->wait_type &= ~WAIT_CONDVAR;
        t->cond_wait_obj = NULL;
        TRACE_THREAD("CONDVAR WAIT: Thread %d cleared wait state for condvar=%p", t->tid, cond);
    }
    spl(sr);
    
    return lock_result;
}

/**
 * Signal one thread waiting on condition variable
 */
int proc_thread_condvar_signal(struct condvar *cond) {
    if (!validate_condvar(cond)) {
        TRACE_THREAD("CONDVAR SIGNAL: Invalid condvar %p", cond);
        return EINVAL;
    }

    TRACE_THREAD("CONDVAR SIGNAL: Signaling one thread on condvar=%p", cond);
    register unsigned short sr = splhigh();
    
    // POSIX compliance: wake first waiting thread (FIFO order)
    struct thread *prev_first = NULL;
    struct thread *first = find_first_waiting_thread(cond->wait_queue, &prev_first);
    
    if (!first) {
        spl(sr);
        TRACE_THREAD("CONDVAR SIGNAL: No threads waiting on condvar=%p", cond);
        return THREAD_SUCCESS;
    }
    
    // Validate thread before waking
    if (first->magic != CTXT_MAGIC) {
        spl(sr);
        TRACE_THREAD("CONDVAR SIGNAL: Invalid thread in wait queue");
        return EINVAL;
    }
    remove_thread_from_condvar_queue(cond, first, prev_first);

    TRACE_THREAD("CONDVAR SIGNAL: Waking thread %d from condvar=%p (FIFO order)", 
                first->tid, cond);
    
    // Wake up the thread
    wakeup_condvar_thread(first);

    spl(sr);

    return THREAD_SUCCESS;
}

/**
 * Broadcast to all threads waiting on condition variable
 */
int proc_thread_condvar_broadcast(struct condvar *cond) {
    if (!validate_condvar(cond)) {
        TRACE_THREAD("CONDVAR BROADCAST: Invalid condvar %p", cond);
        return EINVAL;
    }
    
    register unsigned short sr = splhigh();
    
    struct thread *t = cond->wait_queue;
    if (!t) {
        spl(sr);
        TRACE_THREAD("CONDVAR BROADCAST: No threads waiting on condvar=%p", cond);
        return THREAD_SUCCESS; // No threads waiting, not an error
    }
    
    TRACE_THREAD("CONDVAR BROADCAST: Waking all threads from condvar=%p", cond);
    
    // Wake up all waiting threads
    while (t) {
        struct thread *next = t->next_wait;
        // Validate thread before waking
        if (t->magic == CTXT_MAGIC) {
            t->next_wait = NULL;  // Clear link before waking
            wakeup_condvar_thread(t);
            TRACE_THREAD("CONDVAR BROADCAST: Woke thread %d", t->tid);
        }
        t = next;
    }
    
    // Clear the wait queue
    cond->wait_queue = NULL;
    TRACE_THREAD("CONDVAR BROADCAST: Cleared wait queue for condvar=%p", cond);
    spl(sr);
    return THREAD_SUCCESS;
}

/**
 * Timed wait on condition variable
 */
int proc_thread_condvar_timedwait(struct condvar *cond, struct mutex *mutex, long timeout_ms) {
    if (!validate_condvar(cond) || !mutex || timeout_ms < 0) {
        return EINVAL;
    }

    struct thread *t = CURTHREAD;
    if (!t) {
        TRACE_THREAD("CONDVAR TIMEDWAIT: No current thread");
        return EINVAL;
    }

    // Prevent nested blocking
    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("CONDVAR TIMEDWAIT: Thread %d already blocked", t->tid);
        return EDEADLK;
    }

    // Verify thread owns the mutex
    if (mutex->owner != t) {
        TRACE_THREAD("CONDVAR TIMEDWAIT: Thread %d doesn't own mutex", t->tid);
        return EINVAL;
    }

    TRACE_THREAD("CONDVAR TIMEDWAIT: Thread %d preparing to timed wait on condvar=%p with mutex=%p for %ld ms", 
                t->tid, cond, mutex, timeout_ms);
                
    register unsigned short sr;
    
    // Atomic mutex association
    int assoc_result = associate_mutex_atomic(cond, mutex);
    if (assoc_result != 0) {
        return EINVAL;
    }
    
    TRACE_THREAD("CONDVAR TIMEDWAIT: Thread %d waiting on condvar=%p, timeout=%ld", 
                t->tid, cond, timeout_ms);
    
    // Set up timeout
    TIMEOUT *wait_timeout = NULL;
    if (timeout_ms > 0) {
        wait_timeout = addtimeout(t->proc, timeout_ms, proc_thread_condvar_timeout_handler);
        if (wait_timeout) {
            wait_timeout->arg = (long)t;
        }
    }
    
    // Add to condition variable wait queue
    t->wait_type |= WAIT_CONDVAR;
    t->cond_wait_obj = cond;

    sr = splhigh();

    // Add to wait queue (FIFO order for condition variables)
    struct thread **tqueue = &cond->wait_queue;
    while (*tqueue) tqueue = &(*tqueue)->next_wait;
    *tqueue = t;
    t->next_wait = NULL;
    
    spl(sr);

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    
    
    
    // Release the mutex before blocking
    int unlock_result = thread_mutex_unlock(mutex);
    if (unlock_result != THREAD_SUCCESS) {
        // Remove from wait queue if unlock failed

        sr = splhigh();

        if (wait_timeout) {
            canceltimeout(wait_timeout);
        }
        
        struct thread **pp = &cond->wait_queue;
        while (*pp && *pp != t) pp = &(*pp)->next_wait;
        if (*pp) *pp = t->next_wait;
        
        t->wait_type &= ~WAIT_CONDVAR;
        t->cond_wait_obj = NULL;
        t->next_wait = NULL;

        spl(sr);

        proc_thread_state_change(t, THREAD_STATE_READY);
        
        return unlock_result;
    }
    
    // Block until signaled or timeout
    proc_thread_schedule();
    
    // Cancel timeout if it exists
    sr = splhigh();

    if (wait_timeout) {
        canceltimeout(wait_timeout);
    }
    
    // Check if we timed out
    int timed_out = (t->sleep_reason == 1);
    
    // Clear wait state
    if (t->wait_type & WAIT_CONDVAR) {
        t->wait_type &= ~WAIT_CONDVAR;
        t->cond_wait_obj = NULL;
    }
    
    spl(sr);
    
    // Reacquire the mutex
    int lock_result = thread_mutex_lock(mutex);
    
    // Return appropriate result
    if (timed_out) {
        return ETIMEDOUT;
    }
    
    return lock_result;
}

/**
 * Timeout handler for condition variable timed wait
 */
static void proc_thread_condvar_timeout_handler(PROC *p, long arg) {
    struct thread *t = (struct thread *)arg;
    
    if (!t) {
        TRACE_THREAD("CONDVAR TIMEOUT: NULL thread pointer");
        return;
    }
    
    if (t->magic != CTXT_MAGIC) {
        TRACE_THREAD("CONDVAR TIMEOUT: Invalid thread pointer");
        return;
    }
    
    TRACE_THREAD("CONDVAR TIMEOUT: Thread %d timeout", t->tid);
    
    if ((t->state & THREAD_STATE_BLOCKED) && (t->wait_type & WAIT_CONDVAR)) {
        TRACE_THREAD("CONDVAR TIMEOUT: Thread %d timed out waiting on condvar", t->tid);
        register unsigned short sr = splhigh();
        struct condvar *cond = (struct condvar *)t->cond_wait_obj;
        
        t->sleep_reason = 1; // Timeout
        
        // Remove from condition variable wait queue with validation
        if (cond && cond->magic == CONDVAR_MAGIC) {
            TRACE_THREAD("CONDVAR TIMEOUT: Removing thread %d from condvar=%p wait queue", t->tid, cond);
            struct thread **pp = &cond->wait_queue;
            while (*pp && *pp != t) {
                pp = &(*pp)->next_wait;
            }
            if (*pp) {
                *pp = t->next_wait;
            }
        }
        
        t->wait_type &= ~WAIT_CONDVAR;
        t->cond_wait_obj = NULL;
        t->next_wait = NULL;
        spl(sr);
        // Add to ready queue
        proc_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);
    }
    return;
}

