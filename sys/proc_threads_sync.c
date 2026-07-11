/**
 * @file proc_threads_sync.c
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

#include "proc_threads_sync.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"

/* Forward declarations */
static void cleanup_process_semaphores(struct proc *p);
static void cleanup_process_mutexes(struct proc *p);
static void cleanup_thread_condvar_states(struct proc *p);

/**
 * Detach a thread - mark it as not joinable
 * 
 * @param tid Thread ID to detach
 * @return 0 on success, error code on failure
 */
long proc_thread_detach(long tid)
{
    struct proc *p = curproc;
    struct thread *target = NULL;
    register unsigned short sr;
    
    if (!p)
        return EINVAL;
    
    // Find target thread
    for (target = p->threads; target; target = target->next) {
        TRACE_THREAD("DETACH: Checking thread %d", target->tid);
        if (target->tid == tid){
            break;
        }
    }
    
    if (!target){
        TRACE_THREAD("DETACH: No such thread %d", tid);
        return ESRCH;  // No such thread
    }

    sr = splhigh();
    
    // Check if thread is already joined
    if (target->joined) {
        TRACE_THREAD("DETACH: Thread %d already joined", target->tid);
        spl(sr);
        return EINVAL;  // Thread already joined
    }
    
    // Check if thread is already detached
    if (target->detached) {
        TRACE_THREAD("DETACH: Thread %d already detached", target->tid);
        spl(sr);
        return 0;  // Already detached, not an error
    }
    
    // Mark thread as detached
    target->detached = 1;
    
    // If thread already exited, free its resources
    if (target->state & THREAD_STATE_EXITED) {
        TRACE_THREAD("DETACH: Thread %d already exited, freeing resources", target->tid);
        
        // Handle any thread waiting to join this one
        handle_thread_joining(target, NULL);
        
        // Free resources
        // TRACE_THREAD("DETACH: Calling cleanup_thread_resources for thread %d", target->tid);
        // cleanup_thread_resources(p, target, target->tid);
    }
    
    spl(sr);
    return 0;
}

/**
 * Join a thread - wait for it to terminate
 * 
 * @param tid Thread ID to join
 * @param retval Pointer to store the thread's return value
 * @return 0 on success, error code on failure
 */
long proc_thread_join(long tid, void **retval)
{
    struct proc *p = curproc;
    struct thread *current, *target = NULL;
    register unsigned short sr;
    struct thread *t = NULL;
    int target_found = 0;

    if (!p || !p->current_thread)
        return EINVAL;
    
    current = p->current_thread;

    TRACE_THREAD("JOIN: proc_thread_join called for tid=%ld, retval=%p", tid, retval);

    // Cannot join self - would deadlock
    if (current->tid == tid){
        TRACE_THREAD("JOIN: Cannot join self (would deadlock)");
        return EDEADLK;
    }
#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
    for (t = p->threads; t; t = t->next) {
        TRACE_THREAD("  Thread %d: state=%d, magic=%lx, detached=%d, joined=%d",
                    t->tid, t->state, t->magic, t->detached, t->joined);
    }
#endif
    // Find target thread
    for (target = p->threads; target; target = target->next) {
        if (target->tid == tid){
            break;
        }
    }
    
    if (!target){
        TRACE_THREAD("JOIN: Thread %ld not found", tid);
        return ESRCH;  // Return positive error code
    }
    
    // Check if thread is joinable
    if (target->detached){
        TRACE_THREAD("JOIN: Thread %ld is detached, cannot join", tid);
        return EINVAL;  // Return positive error code
    }
    
    // Check if thread is already joined
    if (target->joined){
        TRACE_THREAD("JOIN: Thread %ld is already joined", tid);
        return EINVAL;  // Thread already joined
    }
    
    // Check if another thread is already joining this thread
    if (target->joiner && target->joiner != current && target->joiner->magic == CTXT_MAGIC) {
        TRACE_THREAD("JOIN: Thread %ld is already being joined by thread %d", 
                    tid, target->joiner->tid);
        return EINVAL;  // Thread is already being joined by another thread
    }

    sr = splhigh();
    
    // Check if thread already exited
    if (target->state & THREAD_STATE_EXITED) {
        // Thread already exited, get return value and return immediately
        if (retval) {
            TRACE_THREAD("JOIN: Thread %ld already exited, getting return value %p", tid, target->retval);
            *retval = target->retval;
        }
        
        // Mark as joined so resources can be freed
        target->joined = 1;
        
        // Free resources now
        // cleanup_thread_resources(p, target, tid);
        remove_thread_from_specific_wait_queue(current, WAIT_JOIN);
        
        // Clear any wait state on the current thread
        current->wait_type &= ~WAIT_JOIN;
        current->join_wait_obj = NULL;
        current->join_retval = NULL;
        
        spl(sr);
        return 0;
    }
    
    // Mark target as being joined by current thread
    target->joiner = current;

    // Mark current thread as waiting for join
    current->wait_type |= WAIT_JOIN;
    current->join_wait_obj = target;
    current->join_retval = retval;  // Store the pointer where to put the return value
    TRACE_THREAD("JOIN: Thread %d waiting for thread %d to exit, join_retval=%p", current->tid, target->tid, retval);
    
    // Block the current thread
    proc_thread_state_change(current, THREAD_STATE_BLOCKED);

    remove_from_ready_queue(current);

    spl(sr);

    CONTEXT *ctx = get_thread_context(current);

    TRACE_THREAD("JOIN - PRE-SAVE - USED CONTEXT: Thread %d context - SSP=%lx, USP=%lx, PC=%lx", current->tid, ctx->ssp, ctx->usp, ctx->pc); 

    if (save_context(ctx) == 0) {

        ctx->regs[0] = 1;

        // memcpy(&current->ctxt[SYSCALL], &current->proc->ctxt[SYSCALL], sizeof(CONTEXT));
        TRACE_THREAD("JOIN: SAVED SYSCALL CONTEXT: Thread %d context - SR=%x, SSP=%lx, USP=%lx, PC=%lx", current->tid, current->ctxt[SYSCALL].sr, current->ctxt[SYSCALL].ssp, current->ctxt[SYSCALL].usp, current->ctxt[SYSCALL].pc);
        // Schedule another thread
        proc_thread_schedule();
        
        // Should never reach here
        TRACE_THREAD("JOIN: ERROR - Returned from proc_thread_schedule() in join path!");
        return -1;
    }

    TRACE_THREAD("JOIN: Second time, GETTING CONTEXT: Thread %d context - SSP=%lx, USP=%lx, PC=%lx", current->tid, ctx->ssp, ctx->usp, ctx->pc);
    TRACE_THREAD("JOIN: Second time context, THREAD SYSCALL context for thread %d SR = %x, SSP=%lx, USP=%lx, PC=%lx", current->tid, current->ctxt[SYSCALL].sr, current->ctxt[SYSCALL].ssp, current->ctxt[SYSCALL].usp, current->ctxt[SYSCALL].pc);                     
    TRACE_THREAD("JOIN: Second time context, PROC SYSCALL context for proc %d SR = %x, SSP=%lx, USP=%lx, PC=%lx", current->proc->pid, current->proc->ctxt[SYSCALL].sr, current->proc->ctxt[SYSCALL].ssp, current->proc->ctxt[SYSCALL].usp, current->proc->ctxt[SYSCALL].pc);
    
    // Second time through - waking up after target thread exited
    sr = splhigh();
    
    // Ensure we're in the RUNNING state
    if (current->state != THREAD_STATE_RUNNING) {
        TRACE_THREAD("JOIN: Thread %d not in RUNNING state after wake, fixing", current->tid);
        proc_thread_state_change(current, THREAD_STATE_RUNNING);
    }

    // Check if we're still waiting for the target thread
    if ((current->wait_type & WAIT_JOIN) && current->join_wait_obj == target) {
        // Check if target has exited
        if (!(target->state & THREAD_STATE_EXITED)) {
            TRACE_THREAD("JOIN: Thread %d woken up but target thread %ld hasn't exited yet", 
                        current->tid, tid);
            
            // Go back to waiting
            proc_thread_state_change(current, THREAD_STATE_BLOCKED);
            
            spl(sr);
            // Schedule another thread and continue waiting
            proc_thread_schedule();
            return 0;  // Will never reach here
        }
    } else {
        // We're no longer waiting for this thread, which means it has been handled
        TRACE_THREAD("JOIN: Thread %d no longer waiting for thread %ld", current->tid, tid);
    }

    // CRITICAL FIX: Make sure target thread still exists and is valid
    for (t = p->threads; t; t = t->next) {
        if (t == target) {
            target_found = 1;
            break;
        }
    }
    
    if (!target_found) {
        // Target thread no longer exists in the thread list
        // This means it has been properly joined and freed
        TRACE_THREAD("JOIN: Target thread %ld no longer exists, join successful", tid);
        
        // Clear wait state
        current->wait_type &= ~WAIT_JOIN;
        current->join_wait_obj = NULL;
        
        spl(sr);
        return 0;
    }

    // Get return value if requested
    if (retval && target && target->magic == CTXT_MAGIC)
        *retval = target->retval;
    
    // Clear wait state
    current->wait_type &= ~WAIT_JOIN;
    current->join_wait_obj = NULL;
    
    TRACE_THREAD("JOIN: Thread %d successfully joined thread %ld", current->tid, tid);
    
    spl(sr);

    // CONTEXT *ctx = get_thread_context(current);
    TRACE_THREAD("JOIN: Before return, CURRENT context for thread %d, SR %x, PC %lx, SSP %lx, USP %lx", current->tid, ctx->sr, ctx->pc, ctx->ssp, ctx->usp);
    TRACE_THREAD("JOIN: Before return, PROC SYSCALL context for thread %d, SR %x, PC %lx, SSP %lx, USP %lx", current->tid, current->proc->ctxt[SYSCALL].sr, current->proc->ctxt[SYSCALL].pc, current->proc->ctxt[SYSCALL].ssp, current->proc->ctxt[SYSCALL].usp);

    return 0;
}

/**
 * Clean up thread synchronization states when a process terminates
 * 
 * This function should be called from the terminate() function in k_exit.c
 * to ensure that all threads waiting on synchronization objects are properly
 * unblocked when a process terminates.
 * 
 * @param p The process being terminated
 */

void cleanup_thread_sync_states(struct proc *p)
{
    if (!p) return;
    
    TRACE_THREAD("CLEANUP: Cleaning up sync objects for process %d", p->pid);
    
    register unsigned short sr = splhigh();
    
    // 1. Clean up process-owned mutexes
    cleanup_process_mutexes(p);
    
    // 2. Clean up process-owned semaphores  
    cleanup_process_semaphores(p);
    
    // 3. Clean up process-owned condition variables
    cleanup_thread_condvar_states(p);
    
    // 4. Only clear wait states for THIS process's threads
    struct thread *t;
    for (t = p->threads; t; t = t->next) {
        if (t->magic == CTXT_MAGIC && t->wait_type != WAIT_NONE) {
            t->wait_type = WAIT_NONE;
            t->sem_wait_obj = NULL;
            t->mutex_wait_obj = NULL;
            t->cond_wait_obj = NULL;
            t->next_wait = NULL;
        }
    }
    
    spl(sr);
}
/**
 * Clean up condition variable states when a process terminates
 */
static void cleanup_thread_condvar_states(struct proc *p)
{
    if (!p) return;
    
    TRACE_THREAD("CLEANUP: Cleaning up condvar states for process %d", p->pid);
    
    register unsigned short sr = splhigh();
    
    // Clean up any threads waiting on condition variables
    struct thread *t;
    for (t = p->threads; t; t = t->next) {
        if (t->magic == CTXT_MAGIC && (t->wait_type & WAIT_CONDVAR)) {
            TRACE_THREAD("CLEANUP: Clearing condvar wait state for thread %d", t->tid);
            
            // Clear wait state
            t->wait_type &= ~WAIT_CONDVAR;
            t->cond_wait_obj = NULL;
            t->next_wait = NULL;
        }
    }
    
    spl(sr);
}

/**
 * Clean up process mutexes
 */
static void cleanup_process_mutexes(struct proc *p)
{
    if (!p) return;
    
    TRACE_THREAD("CLEANUP: Cleaning up mutexes for process %d", p->pid);
    
    // Only clean up threads in THIS process that are waiting on mutexes
    struct thread *t;
    for (t = p->threads; t; t = t->next) {
        if (t->magic == CTXT_MAGIC && (t->wait_type & WAIT_MUTEX)) {
            TRACE_THREAD("CLEANUP: Clearing mutex wait state for thread %d", t->tid);
            
            // Clear the wait state - the mutex will be freed with the process
            t->wait_type &= ~WAIT_MUTEX;
            t->mutex_wait_obj = NULL;
            t->next_wait = NULL;
        }
    }
    
    // Note: We don't need to iterate through other processes because
    // mutexes are process-private and cannot be shared between processes
}

/**
 * Clean up process semaphores
 */
static void cleanup_process_semaphores(struct proc *p)
{
    if (!p) return;
    
    TRACE_THREAD("CLEANUP: Cleaning up semaphores for process %d", p->pid);
    
    // Only clean up threads in THIS process that are waiting on semaphores
    struct thread *t;
    for (t = p->threads; t; t = t->next) {
        if (t->magic == CTXT_MAGIC && (t->wait_type & WAIT_SEMAPHORE)) {
            TRACE_THREAD("CLEANUP: Clearing semaphore wait state for thread %d", t->tid);
            
            // Clear the wait state - the semaphore will be freed with the process
            t->wait_type &= ~WAIT_SEMAPHORE;
            t->sem_wait_obj = NULL;
            t->next_wait = NULL;
        }
    }
}

/* Initialize RWLock - returns handle ID for userspace */
long thread_rwlock_init(void) {
    struct rwlock *rw = kmalloc(sizeof(struct rwlock));
    if (!rw) 
        return -ENOMEM;
    
    int result = thread_mutex_init(&rw->lock, NULL);
    if (result != THREAD_SUCCESS) {
        kfree(rw);
        return -result;
    }
    
    result = proc_thread_condvar_init(&rw->readers_ok);
    if (result != 0) {
        kfree(rw);
        return -result;
    }
    
    result = proc_thread_condvar_init(&rw->writers_ok);
    if (result != 0) {
        proc_thread_condvar_destroy(&rw->readers_ok);
        kfree(rw);
        return -result;
    }
    
    rw->readers = 0;
    rw->writers = 0;
    rw->waiting_writers = 0;
    rw->waiting_readers = 0;
    
    // In a real implementation, you'd want to store this in a handle table
    // For now, return the pointer as a handle (not safe for production)
    return (long)rw;
}

/* Destroy RWLock */
long thread_rwlock_destroy(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) 
        return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    // Check for active users
    if (rw->readers || rw->writers || rw->waiting_writers || rw->waiting_readers) {
        thread_mutex_unlock(&rw->lock);
        return -EBUSY;
    }
    
    thread_mutex_unlock(&rw->lock);
    
    proc_thread_condvar_destroy(&rw->readers_ok);
    proc_thread_condvar_destroy(&rw->writers_ok);
    thread_mutex_destroy(&rw->lock);
    kfree(rw);
    return 0;
}

/* Reader Lock */
long thread_rwlock_rdlock(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    // Writer-preference: wait if writers active or waiting
    while (rw->writers > 0 || rw->waiting_writers > 0) {
        rw->waiting_readers++;
        proc_thread_condvar_wait(&rw->readers_ok, &rw->lock);
        rw->waiting_readers--;
    }
    
    rw->readers++;
    thread_mutex_unlock(&rw->lock);
    return 0;
}

/* Try Reader Lock (non-blocking) */
long thread_rwlock_tryrdlock(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    // Check if writers active or waiting
    if (rw->writers > 0 || rw->waiting_writers > 0) {
        thread_mutex_unlock(&rw->lock);
        return -EBUSY;
    }
    
    rw->readers++;
    thread_mutex_unlock(&rw->lock);
    return 0;
}

/* Writer Lock */
long thread_rwlock_wrlock(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    // Wait while readers active or another writer active
    while (rw->readers > 0 || rw->writers > 0) {
        rw->waiting_writers++;
        proc_thread_condvar_wait(&rw->writers_ok, &rw->lock);
        rw->waiting_writers--;
    }
    
    rw->writers = 1;
    thread_mutex_unlock(&rw->lock);
    return 0;
}

/* Try Writer Lock (non-blocking) */
long thread_rwlock_trywrlock(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    // Check if readers or writers active
    if (rw->readers > 0 || rw->writers > 0) {
        thread_mutex_unlock(&rw->lock);
        return -EBUSY;
    }
    
    rw->writers = 1;
    thread_mutex_unlock(&rw->lock);
    return 0;
}

/* Unlock RWLock */
long thread_rwlock_unlock(long handle) {
    struct rwlock *rw = (struct rwlock *)handle;
    if (!rw) return -EINVAL;
    
    thread_mutex_lock(&rw->lock);
    
    if (rw->writers) {
        // Writer unlocking
        rw->writers = 0;
        if (rw->waiting_writers > 0) {
            // Prioritize waiting writers
            proc_thread_condvar_signal(&rw->writers_ok);
        } else if (rw->waiting_readers > 0) {
            // Wake all waiting readers
            proc_thread_condvar_broadcast(&rw->readers_ok);
        }
    } else if (rw->readers > 0) {
        // Reader unlocking
        rw->readers--;
        if (rw->readers == 0 && rw->waiting_writers > 0) {
            // Last reader wakes waiting writer
            proc_thread_condvar_signal(&rw->writers_ok);
        }
    } else {
        // Lock not held
        thread_mutex_unlock(&rw->lock);
        return -EPERM;
    }
    
    thread_mutex_unlock(&rw->lock);
    return 0;
}