/**
 * @file proc_threads_mutex.c
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

static inline void mutex_priority_ceiling(struct mutex *mutex, struct thread *t, int acquire)
{
    /* body unchanged -- kept as the actual PRIO_PROTECT logic */
    if (acquire) {
        mutex->saved_priority = t->priority;
        t->priority = mutex->prioceiling;
    } else {
        t->priority = mutex->saved_priority;
    }
    TRACE_THREAD("MUTEX CEILING: Set thread %d priority from %d to %d", t->tid, mutex->saved_priority, t->priority);
}

/* Skip the call entirely unless PRIO_PROTECT is actually in use --
 * this is the overwhelmingly common case (SDL2 et al. use PRIO_NONE),
 * so the guard belongs at the call site, not inside the callee. */
#define MUTEX_CEILING(mutex, t, acquire) \
    do { if ((mutex)->protocol == PTHREAD_PRIO_PROTECT) \
             mutex_priority_ceiling((mutex), (t), (acquire)); } while (0)

/* Same idea for reset_thread_priority(): it already early-returns
 * internally when the thread was never boosted, but it's an external
 * (non-static) function, so the compiler can't fold that guard into
 * the call site. Checking priority_boost here avoids the jsr/rts pair
 * entirely in the common unboosted case. */
#define RESET_PRIORITY(t) \
    do { if ((t)->priority_boost) reset_thread_priority(t); } while (0)

/* Mutex attribute functions */
int thread_mutexattr_init(struct mutex_attr *attr) {
    if (!attr)
        return EINVAL;

    attr->type = PTHREAD_MUTEX_NORMAL;
    attr->pshared = 0; /* Default to process-private */
    attr->protocol = PTHREAD_PRIO_NONE;
    if (CURTHREAD) {
        attr->prioceiling = CURTHREAD->priority;  /* Default to current thread's priority */
    } else {
        attr->prioceiling = 1;  /* Default priority if no current thread */
    }
    return THREAD_SUCCESS;
}

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
        if (CURTHREAD) {
            mutex->prioceiling = (int)CURTHREAD->priority;  /* Default to current thread's priority */
        } else {
            mutex->prioceiling = 1;  /* Default priority if no current thread */
        }
    }
    TRACE_THREAD("MUTEX INIT: Initialized mutex %p with type %d, protocol %d, prioceiling %d",
                 mutex, mutex->type, mutex->protocol, mutex->prioceiling);
    return THREAD_SUCCESS;
}

int thread_mutex_lock(struct mutex *mutex) {
    struct thread *t;
    register unsigned short sr;
    short tprio;

    if (!mutex) return EINVAL;
    t = CURTHREAD;
    if (!t) return EINVAL;

    sr = splhigh();

    /* Fast path first: this is the branch that runs on every
     * uncontended call, keep it minimal -- 3 stores, no calls. */
    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = t;
        mutex->lock_count = 1;
        MUTEX_CEILING(mutex, t, 1);
        spl(sr);
        return THREAD_SUCCESS;
    }

    if (mutex->owner == t) {
        /* NORMAL and ERRORCHECK collapse to the same result --
         * one branch instead of a 3-way switch. */
        int ret = EDEADLK;
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
            mutex->lock_count++;
            ret = THREAD_SUCCESS;
        }
        spl(sr);
        return ret;
    }

    if (t->wait_type != WAIT_NONE) {
        spl(sr);
        return EDEADLK;
    }

    t->wait_type |= WAIT_MUTEX;
    t->mutex_wait_obj = mutex;
    tprio = t->priority;              /* cache: read once, used twice below */

    {
        struct thread **pp = &mutex->wait_queue;
        while (*pp && (*pp)->priority > tprio)
            pp = &(*pp)->next_wait;
        t->next_wait = *pp;
        *pp = t;
    }

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);

    {
        struct thread *owner = mutex->owner;   /* one deref instead of three */
        if (owner && owner->priority < tprio) {
            boost_thread_priority(owner, tprio - owner->priority);
            if (owner->state == THREAD_STATE_READY) {
                remove_from_ready_queue(owner);
                add_to_ready_queue(owner);
            }
        }
    }

    spl(sr);                          /* don't hold IPL across the yield */

    TRACE_THREAD_VERBOSE("MUTEX LOCK: Calling proc_thread_schedule()");
    proc_thread_schedule();
    TRACE_THREAD_VERBOSE("MUTEX LOCK: Back from proc_thread_schedule()");

    if (t->wakeup_time > 0) {
        t->wakeup_time = 0;
        remove_from_sleep_queue(t->proc, t);
    }

    sr = splhigh();
    if (mutex->owner != t) {
        mutex->owner = t;
        mutex->locked = 1;
        mutex->lock_count = 1;
    }
    spl(sr);

    return THREAD_SUCCESS;
}

int thread_mutex_unlock(struct mutex *mutex) {
    struct thread *current;
    register unsigned short sr;
    struct thread *highest = NULL;

    if (!mutex) {
        TRACE_THREAD("MUTEX UNLOCK: NULL mutex");
        return EINVAL;
    }
    current = CURTHREAD;
    if (!current) {
        TRACE_THREAD("MUTEX UNLOCK: NULL current thread");
        return EINVAL;
    }

    if (mutex->owner != current) {
        TRACE_THREAD("MUTEX UNLOCK: Not owner of mutex");
        return EPERM;
    }

    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->lock_count > 1) {
        sr = splhigh();
        TRACE_THREAD("MUTEX UNLOCK: Recursive lock count %d, decrementing", mutex->lock_count);
        mutex->lock_count--;
        spl(sr);
        return THREAD_SUCCESS;
    }

    /* wait_queue splice must be atomic against a timer interrupt,
     * same as lock() -- this section was unguarded originally. */
    sr = splhigh();

    MUTEX_CEILING(mutex, current, 0);

    if (mutex->wait_queue) {
        struct thread *prev = NULL;
        highest = find_highest_priority_thread_in_queue(mutex->wait_queue, &prev);

        TRACE_THREAD("MUTEX UNLOCK: highest priority thread %p", highest);

        if (highest) {
            if (prev) prev->next_wait = highest->next_wait;
            else      mutex->wait_queue = highest->next_wait;
            highest->next_wait = NULL;

            mutex->owner = highest;
            mutex->lock_count = 1;

            MUTEX_CEILING(mutex, highest, 1);

            highest->wait_type &= ~WAIT_MUTEX;
            highest->mutex_wait_obj = NULL;

            if (highest->wakeup_time > 0) {
                remove_from_sleep_queue(highest->proc, highest);
                highest->wakeup_time = 0;
            }

            proc_thread_state_change(highest, THREAD_STATE_READY);
            add_to_ready_queue(highest);
        }
    }

    if (!highest) {
        TRACE_THREAD("MUTEX UNLOCK: No highest priority thread");
        mutex->locked = 0;
        mutex->owner = NULL;
        mutex->lock_count = 0;
    }
    RESET_PRIORITY(current);   /* single call site instead of two, now guarded */

    spl(sr);

    if (highest && highest->priority > current->priority){
        TRACE_THREAD_VERBOSE("MUTEX UNLOCK: Calling proc_thread_schedule()");
        proc_thread_schedule();
        TRACE_THREAD_VERBOSE("MUTEX UNLOCK: Back from proc_thread_schedule()");
    }
    
    TRACE_THREAD("MUTEX UNLOCK: Success");
    return THREAD_SUCCESS;
}

/**
 * Non-blocking mutex lock attempt
 */
int thread_mutex_trylock(struct mutex *mutex) {
    struct thread *t;
    register unsigned short sr;

    if (!mutex) {
        return EINVAL;
    }

    t = CURTHREAD;

    if (!t) {
        TRACE_THREAD("THREAD_MUTEX_TRYLOCK: No current thread");
        return EINVAL;
    }

    sr = splhigh();

    /* Check if mutex is free */
    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = t;
        mutex->lock_count = 1;

        /* Apply priority ceiling if needed */
        MUTEX_CEILING(mutex, t, 1);

        spl(sr);
        return THREAD_SUCCESS;
    }

    /* Check for recursive locking -- NORMAL and ERRORCHECK collapse
     * to the same result, same as lock(). */
    if (mutex->owner == t) {
        int ret = EDEADLK;
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
            TRACE_THREAD("MUTEX TRYLOCK: Thread %d re-acquired recursive lock", t->tid);
            mutex->lock_count++;
            ret = THREAD_SUCCESS;
        } else {
            TRACE_THREAD("MUTEX TRYLOCK: Thread %d tried to re-lock mutex %p", t->tid, mutex);
        }
        spl(sr);
        return ret;
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
 * Destroy a mutex - POSIX compliant
 */
int thread_mutex_destroy(struct mutex *mutex) {
    register unsigned short sr;

    if (!mutex) {
        return EINVAL;
    }

    sr = splhigh();

    /* POSIX: Destroying a locked mutex is undefined behavior.
     * Some implementations return EBUSY, some do nothing.
     * We'll follow the common Linux behavior: return EBUSY if locked.
     */
    if (mutex->locked) {
        struct thread *current = CURTHREAD;

        TRACE_THREAD("MUTEX DESTROY: mutex=%p is locked (owner=%d) - POSIX says undefined",
                     mutex, mutex->owner ? mutex->owner->tid : -1);

        if (mutex->type == PTHREAD_MUTEX_RECURSIVE &&
            mutex->owner == current) {
            /* For a recursive mutex owned by the current thread, safe
             * to destroy by dropping all levels at once -- lock_count
             * is discarded unconditionally right after, no need to
             * walk it down one decrement at a time. */
            TRACE_THREAD("MUTEX DESTROY: Unlocking recursive mutex %p (count=%d)",
                         mutex, mutex->lock_count);

            mutex->lock_count = 0;
            mutex->locked = 0;
            mutex->owner = NULL;

            /* Continue with destroy... */
        } else {
            /* Standard case: return EBUSY (common but not required by POSIX) */
            spl(sr);
            return EBUSY;
        }
    }

    /* Check for waiters - destroying with waiters is also undefined */
    if (mutex->wait_queue) {
        TRACE_THREAD("MUTEX DESTROY: mutex=%p has waiters - POSIX says undefined",
                     mutex);

        /* We could wake all waiters with EINVAL, but that's not required */
        spl(sr);
        return EBUSY;
    }

    /* Safe to destroy - clear all fields */
    TRACE_THREAD("MUTEX DESTROY: Successfully destroying mutex %p", mutex);
    mint_bzero(mutex, sizeof(struct mutex));

    spl(sr);
    return THREAD_SUCCESS;
}