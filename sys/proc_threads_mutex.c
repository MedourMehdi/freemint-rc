/**
 * @file proc_threads_sync.c
 * @brief Kernel-level Thread Synchronization
 * 
 * Implements core synchronization primitives (mutexes) within the FreeMiNT kernel.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads_mutex.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"


static void handle_priority_ceiling(struct mutex *mutex, struct thread *t, int acquire);

/* Mutex attribute functions */
int thread_mutexattr_init(struct mutex_attr *attr) {
    if (!attr) 
        return EINVAL;
    
    attr->type = PTHREAD_MUTEX_NORMAL;
    attr->pshared = 0; /* Default to process-private */
    attr->protocol = PTHREAD_PRIO_NONE;
    if(CURTHREAD) {
        attr->prioceiling = CURTHREAD->priority;  // Default to current thread's priority
    } else {
        attr->prioceiling = MAX(scale_thread_priority(-curproc->pri), 1);  // Default to process's priority if no current thread
    }
    return THREAD_SUCCESS;
}

/* Handle priority ceiling protocol */
static void handle_priority_ceiling(struct mutex *mutex, struct thread *t, int acquire) {
    if (!mutex || !t) 
        return;
    
    if (mutex->protocol != PTHREAD_PRIO_PROTECT) 
        return;
    
    if (acquire) {
        /* Save original priority and set to ceiling */
        mutex->saved_priority = t->priority;
        t->priority = mutex->prioceiling;
        TRACE_THREAD("PRI-CEILING: Thread %d priority set to %d", 
                    t->tid, mutex->prioceiling);
    } else {
        /* Restore original priority */
        t->priority = mutex->saved_priority;
        TRACE_THREAD("PRI-CEILING: Thread %d priority restored to %d", 
                    t->tid, mutex->saved_priority);
    }
    
}

// Function to initialize a mutex
int thread_mutex_init(struct mutex *mutex, const struct mutex_attr *attr) {
    if (!mutex) {
        return EINVAL;
    }
    
    mutex->locked = 0;
    mutex->owner = NULL;
    mutex->wait_queue = NULL;
    mutex->lock_count = 0;
    
    /* Apply attributes or set defaults */
    if (attr) {
        mutex->type = attr->type;
        mutex->protocol = attr->protocol;
        mutex->prioceiling = attr->prioceiling;
    } else {
        mutex->type = PTHREAD_MUTEX_NORMAL;
        mutex->protocol = PTHREAD_PRIO_NONE;
        if(CURTHREAD) {
            mutex->prioceiling = (int)CURTHREAD->priority;  // Default to current thread's priority
        } else {
            mutex->prioceiling = (int)MAX(scale_thread_priority(-curproc->pri), 1);  // Default to process's priority if no current thread
        }
    }
    return THREAD_SUCCESS;
}

int thread_mutex_lock(struct mutex *mutex) {
    if (!mutex) {
        TRACE_THREAD("THREAD_MUTEX_LOCK: mutex is NULL");
        return EINVAL;
    }

    struct thread *t = CURTHREAD;
    if (!t) {
        TRACE_THREAD("THREAD_MUTEX_LOCK: current thread is NULL");
        return EINVAL;
    }

    register unsigned short sr = splhigh();

    TRACE_THREAD("MUTEX: Thread %d attempting to lock mutex %p (owner=%d)", 
                 t->tid, mutex, 
                 mutex->owner ? mutex->owner->tid : -1);

    /* Check if mutex is free */
    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = t;
        mutex->lock_count = 1;
            
        /* Apply priority ceiling if needed */
        handle_priority_ceiling(mutex, t, 1);
        TRACE_THREAD("MUTEX LOCK: Thread %d acquired mutex %p, mutex owner pointer is now %p", t->tid, mutex, mutex->owner);
        spl(sr);
        return THREAD_SUCCESS;
    }

    TRACE_THREAD("MUTEX LOCK: Mutex %p locked count is %d", mutex, mutex->lock_count);

    /* Check for recursive locking */
    if (mutex->owner == t) {
        switch (mutex->type) {
            case PTHREAD_MUTEX_RECURSIVE:
                TRACE_THREAD("MUTEX LOCK: Thread %d re-acquired recursive lock", t->tid);
                mutex->lock_count++;
                spl(sr);
                return THREAD_SUCCESS;
                
            case PTHREAD_MUTEX_ERRORCHECK:
                TRACE_THREAD("MUTEX LOCK: Thread %d tried to re-lock mutex %p", t->tid, mutex);
                spl(sr);
                return EDEADLK;
                
            default: /* PTHREAD_MUTEX_NORMAL */
                TRACE_THREAD("MUTEX LOCK: Thread %d tried to re-lock normal mutex %p", t->tid, mutex);
                spl(sr);
                return EDEADLK;
        }
    }

    /* Prevent nested blocking */
    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("MUTEX LOCK: Thread %d already blocked", t->tid);
        spl(sr);
        return EDEADLK;
    }

    TRACE_THREAD_MUTEX("locking", t, mutex);
    
    /* Add to wait queue with priority ordering */
    t->wait_type |= WAIT_MUTEX;
    t->mutex_wait_obj = mutex;
    
    /* Insert in priority order (higher priority first) */
    struct thread **pp = &mutex->wait_queue;
    while (*pp && (*pp)->priority > t->priority) {
        pp = &(*pp)->next_wait;
    }
    t->next_wait = *pp;
    *pp = t;
    
    TRACE_THREAD("THREAD_MUTEX_LOCK: Block thread %d", t->tid);
    atomic_thread_state_change(t, THREAD_STATE_BLOCKED);
    
    /* Priority inheritance - boost the priority of the mutex owner */
    if (mutex->owner && mutex->owner->priority < t->priority) {
        TRACE_THREAD("PRI-INHERIT: Thread %d (pri %d) -> owner %d (pri %d)",
                    t->tid, t->priority,
                    mutex->owner->tid, mutex->owner->priority);
        
        /* Boost owner's priority to the waiting thread's priority */
        boost_thread_priority(mutex->owner, t->priority - mutex->owner->priority);
        
        /* Reinsert owner in ready queue if needed */
        if (mutex->owner->state == THREAD_STATE_READY) {
            remove_from_ready_queue(mutex->owner);
            add_to_ready_queue(mutex->owner);
        }
    }

    spl(sr);
    
    /* Yield CPU - will resume here when woken */
    proc_thread_schedule();
    
    /* When we resume, check if we were sleeping */
    sr = splhigh();
    if (t->wakeup_time > 0) {
        TRACE_THREAD("THREAD_MUTEX_LOCK: Thread %d was sleeping, clearing sleep state", t->tid);
        t->wakeup_time = 0;
        remove_from_sleep_queue(t->proc, t);
    }
    
    /* When we resume, we should own the lock */
    if (mutex->owner != t) {
        TRACE_THREAD("THREAD_MUTEX_LOCK: Thread %d woke up but doesn't own mutex!", t->tid);
        mutex->owner = t;
        mutex->locked = 1;
        mutex->lock_count = 1;
    }
    
    spl(sr);
    return THREAD_SUCCESS;

}

int thread_mutex_unlock(struct mutex *mutex) {
    if (!mutex) {
        return EINVAL;
    }
    
    struct thread *current = CURTHREAD;
    if (!current) {
        return EINVAL;
    }
    
    register unsigned short sr = splhigh();
    
    /* Check if current thread owns the mutex */
    if (mutex->owner != current) {
        TRACE_THREAD("THREAD_MUTEX_UNLOCK: Thread %d is not the owner (owner=%d)", 
                    current->tid, mutex->owner ? mutex->owner->tid : -1);
        spl(sr);
        return (mutex->type == PTHREAD_MUTEX_ERRORCHECK) ? EPERM : EPERM;
    }
    
    /* Handle recursive unlocking */
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->lock_count > 1) {
        TRACE_THREAD("THREAD_MUTEX_UNLOCK: Thread %d releasing recursive lock (count=%d)", 
                    current->tid, mutex->lock_count);
        mutex->lock_count--;
        spl(sr);
        return THREAD_SUCCESS;
    }    
    
    /* Remove priority ceiling */
    handle_priority_ceiling(mutex, current, 0);
    
    /* If there are waiters, wake the highest priority one */
    if (mutex->wait_queue) {
        struct thread *prev_highest = NULL;
        struct thread *highest = find_highest_priority_thread_in_queue(mutex->wait_queue, &prev_highest);
        
        if (highest) {
            /* Remove from wait queue */
            if (prev_highest) {
                prev_highest->next_wait = highest->next_wait;
            } else {
                mutex->wait_queue = highest->next_wait;
            }
            highest->next_wait = NULL;
            
            TRACE_THREAD("THREAD_MUTEX_UNLOCK: Waking thread %d (priority %d)", 
                        highest->tid, highest->priority);
            
            /* Transfer lock ownership */
            mutex->owner = highest;
            mutex->lock_count = 1;
            
            /* Apply priority ceiling to new owner */
            handle_priority_ceiling(mutex, highest, 1);
            
            /* Clear wait state */
            highest->wait_type &= ~WAIT_MUTEX;
            highest->mutex_wait_obj = NULL;
            
            /* Remove from sleep queue if needed */
            if (highest->wakeup_time > 0) {
                remove_from_sleep_queue(highest->proc, highest);
                highest->wakeup_time = 0;
            }
            
            /* Mark as ready and add to ready queue */
            atomic_thread_state_change(highest, THREAD_STATE_READY);
            add_to_ready_queue(highest);

            /* Restore original priority if boosted */
            reset_thread_priority(current);
            
            /* Force immediate scheduling if higher priority */
            if (highest->priority > current->priority) {
                TRACE_THREAD("THREAD_MUTEX_UNLOCK: Forcing immediate schedule due to priority");
                spl(sr);
                proc_thread_schedule();
                return THREAD_SUCCESS;
            }
            
            spl(sr);
            return THREAD_SUCCESS;
        }
    }
    
    /* No waiters, release the mutex */
    TRACE_THREAD_MUTEX("unlocking", current, mutex);
    mutex->locked = 0;
    mutex->owner = NULL;
    mutex->lock_count = 0;
    
    /* Restore original priority if boosted */
    reset_thread_priority(current);
    
    spl(sr);
    return THREAD_SUCCESS;
}

/**
 * Non-blocking mutex lock attempt
 */
int thread_mutex_trylock(struct mutex *mutex) {
    if (!mutex) {
        return EINVAL;
    }

    struct thread *t = CURTHREAD;
    if (!t) {
        return EINVAL;
    }

    register unsigned short sr = splhigh();

    /* Check if mutex is free */
    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = t;
        mutex->lock_count = 1;
        
        /* Apply priority ceiling if needed */
        handle_priority_ceiling(mutex, t, 1);
        
        spl(sr);
        return THREAD_SUCCESS;
    }
    
    /* Check for recursive locking */
    if (mutex->owner == t) {
        switch (mutex->type) {
            case PTHREAD_MUTEX_RECURSIVE:
                TRACE_THREAD("MUTEX TRYLOCK: Thread %d re-acquired recursive lock", t->tid);
                mutex->lock_count++;
                spl(sr);
                return THREAD_SUCCESS;
                
            case PTHREAD_MUTEX_ERRORCHECK:
                TRACE_THREAD("MUTEX TRYLOCK: Thread %d tried to re-lock mutex %p", t->tid, mutex);
                spl(sr);
                return EDEADLK;
                
            default: /* PTHREAD_MUTEX_NORMAL */
                TRACE_THREAD("MUTEX TRYLOCK: Thread %d tried to re-lock normal mutex %p", t->tid, mutex);
                spl(sr);
                return EDEADLK;
        }
    }
    
    /* Mutex is locked by someone else */
    spl(sr);
    return EBUSY;
}

int thread_mutexattr_destroy(struct mutex_attr *attr) {
    if (!attr)
        return EINVAL;

    mint_bzero(attr, sizeof(*attr));
    return THREAD_SUCCESS;
}

/**
 * Destroy a mutex
 */
int thread_mutex_destroy(struct mutex *mutex) {
    if (!mutex) {
        return EINVAL;
    }

    register unsigned short sr = splhigh();

    // Check if mutex is locked or has waiters
    if (mutex->locked || mutex->wait_queue) {
        TRACE_THREAD("MUTEX DESTROY: mutex=%p is locked or has waiters", mutex);
        spl(sr);
        return EBUSY;
    }

    // Reset mutex state
    mutex->locked = 0;
    mutex->owner = NULL;
    mutex->lock_count = 0;
    spl(sr);
    return THREAD_SUCCESS;
}