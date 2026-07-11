/**
 * @file proc_threads_queue.c
 * @brief Thread Queue Implementation
 * 
 * Implements queue management for thread scheduling and synchronization.
 * 
 * Features:
 *  - Priority-based ready queue insertion
 *  - Atomic queue operations with interrupt protection
 *  - Wait queue management for synchronization objects
 *  - Sleep queue for timed thread suspension
 *  - Bitmap-optimized priority scanning
 *  - O(1) ready queue membership test using in_ready_queue flag
 * 
 * Ensures POSIX-compliant ordering and efficient scheduling operations.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#include "proc_threads_queue.h"
#include "proc_threads_helper.h"
#include "proc_threads_sync.h"
#include "proc_threads_sem.h"

void add_to_ready_queue(struct thread *t) {
    struct proc *p;
    register unsigned short sr;
    
    /* Single validation check */
    if (!t || !t->proc || 
        (t->state & (THREAD_STATE_RUNNING | THREAD_STATE_EXITED))) {
        TRACE_THREAD("READY_Q: Invalid thread %d state=%d, not adding to ready queue", 
                     t ? t->tid : -1, t ? t->state : -1);
        return;
    }
    
    p = t->proc;
    
    /* Set state BEFORE checking queue membership */
    proc_thread_state_change(t, THREAD_STATE_READY);
    
    sr = splhigh();

    /* Fast O(1) check using flag */
    if (t->in_ready_queue) {
        TRACE_THREAD("Ready_Q: Thread %d already in ready queue", t->tid);
        spl(sr);
        return;
    }
    
    /* Mark as in ready queue before adding */
    t->in_ready_queue = 1;
    
    TRACE_THREAD("READY_Q: Adding Thread %d (pri %d, policy %d, boost=%d), Proc PID %d",
                t->tid, t->priority, t->policy, t->priority_boost, p->pid);
    
    /* If ready queue is empty, just add the thread */
    if (!p->ready_queue) {
        TRACE_THREAD("READY_Q: Empty queue, adding thread %d as head", t->tid);
        p->ready_queue = t;
        t->next_ready = NULL;
        spl(sr);
        return;
    }
    
    /* Priority boosted threads: add in strict priority order */
    if (t->priority_boost) {
        TRACE_THREAD("READY_Q: Thread %d has boosted priority %d", 
                    t->tid, t->priority);
                    
        /* Find position based on priority */
        struct thread **pp = &p->ready_queue;
        while (*pp && (*pp)->priority > t->priority) {
            pp = &(*pp)->next_ready;
        }
        
        /* For equal priority, ensure FIFO order by TID */
        while (*pp && (*pp)->priority == t->priority && (*pp)->tid < t->tid) {
            pp = &(*pp)->next_ready;
        }
        TRACE_THREAD("READY_Q: Inserting thread %d before thread %d", 
                    t->tid, *pp ? (*pp)->tid : -1);
        /* Insert at the right position */
        t->next_ready = *pp;
        *pp = t;
    }
    /* Default: add to end of queue */
    else {
        TRACE_THREAD("READY_Q: Thread %d normal priority %d, adding to end", 
                    t->tid, t->priority);
        struct thread *last = p->ready_queue;
        while (last->next_ready) {
            last = last->next_ready;
        }
        last->next_ready = t;
        t->next_ready = NULL;
    }
    
    spl(sr);
}

void remove_from_ready_queue(struct thread *t) {
    TRACE_THREAD("READY_Q: Attempting to remove thread %d from ready queue", t->tid);
    TRACE_THREAD("READY_Q: Thread state=%d wait_type=%d", t->state, t->wait_type);
    
    if (!t || !t->proc) return;
    struct proc *p = t->proc;
    register unsigned short sr = splhigh();

    if (!p->ready_queue) {
        TRACE_THREAD("READY_Q: Ready queue empty, cannot remove thread %d", t->tid);
        spl(sr);
        return;
    }

    struct thread **pp = &p->ready_queue;
    struct thread *curr = p->ready_queue;
    int found = 0;

    while (curr) {
        if (curr == t) {
            *pp = curr->next_ready;
            curr->next_ready = NULL;
            curr->in_ready_queue = 0;  /* Clear flag */
            found = 1;
            TRACE_THREAD("READY_Q: Removed thread %d from ready queue", t->tid);
            break;
        }
        pp = &(curr->next_ready);
        curr = curr->next_ready;
    }

    if (!found) {
        TRACE_THREAD("READY_Q: Thread %d not found in ready queue", t->tid);
    }

    spl(sr);
}

/**
 * Remove a thread from its process's sleep queue
 * 
 * @param p Process containing the sleep queue
 * @param t Thread to remove from sleep queue
 */
void remove_from_sleep_queue(struct proc *p, struct thread *t) {
    if (!p || !t) return;
    
    struct thread **pp = &p->sleep_queue;
    register unsigned short sr = splhigh();

    while (*pp) {
        if (*pp == t) {
            *pp = t->next_sleeping;
            t->next_sleeping = NULL;
            TRACE_THREAD("SLEEP_Q: Removed thread %d from sleep queue", t->tid);
            break;
        }
        pp = &(*pp)->next_sleeping;
    }
    // if(curproc != p) make_process_eligible(p);
    spl(sr);
}

/**
 * Legacy function - now calls specific cleanup
 * Only use this for process termination cleanup
 */
void remove_thread_from_wait_queues(struct thread *t) {
    /* Only use for complete cleanup (process termination) */
    remove_thread_from_specific_wait_queue(t, WAIT_MUTEX | WAIT_SEMAPHORE | WAIT_CONDVAR | WAIT_SIGNAL | WAIT_JOIN | WAIT_SLEEP);
}

/**
 * Remove thread from specific wait queue type
 * This is safer than removing from all queues
 */
void remove_thread_from_specific_wait_queue(struct thread *t, int wait_type_mask) {
    if (!t) return;
    
    register unsigned short sr = splhigh();
    
    /* WAIT_MUTEX */
    if ((wait_type_mask & WAIT_MUTEX) && (t->wait_type & WAIT_MUTEX) && t->mutex_wait_obj) {
        struct mutex *m = (struct mutex *)t->mutex_wait_obj;
        struct thread **pp = &m->wait_queue;
        while (*pp) {
            if (*pp == t) {
                TRACE_THREAD("Removing thread %d from mutex wait queue", t->tid);
                *pp = (*pp)->next_wait;
                break;
            }
            pp = &((*pp)->next_wait);  /* Correct pointer advancement */
        }
        t->wait_type &= ~WAIT_MUTEX;
        t->next_wait = NULL;
        t->mutex_wait_obj = NULL;
    }
    
    /* WAIT_SEMAPHORE */
    if ((wait_type_mask & WAIT_SEMAPHORE) && (t->wait_type & WAIT_SEMAPHORE) && t->sem_wait_obj) {
        struct semaphore *sem = (struct semaphore *)t->sem_wait_obj;
        struct thread **pp = &sem->wait_queue;
        while (*pp) {
            if (*pp == t) {
                TRACE_THREAD("Removing thread %d from semaphore wait queue", t->tid);
                *pp = (*pp)->next_wait;
                break;
            }
            pp = &((*pp)->next_wait);
        }
        t->wait_type &= ~WAIT_SEMAPHORE;
        t->next_wait = NULL;
        t->sem_wait_obj = NULL;
    }
    
    /* WAIT_CONDVAR */
    if ((wait_type_mask & WAIT_CONDVAR) && (t->wait_type & WAIT_CONDVAR) && t->cond_wait_obj) {
        struct condvar *cond = (struct condvar *)t->cond_wait_obj;
        struct thread **pp = &cond->wait_queue;
        while (*pp) {
            if (*pp == t) {
                TRACE_THREAD("Removing thread %d from condvar wait queue", t->tid);
                *pp = (*pp)->next_wait;
                break;
            }
            pp = &((*pp)->next_wait);
        }
        t->wait_type &= ~WAIT_CONDVAR;
        t->next_wait = NULL;
        t->cond_wait_obj = NULL;
    }

    /* WAIT_SIGNAL - pointer traversal */
    if ((wait_type_mask & WAIT_SIGNAL) && (t->wait_type & WAIT_SIGNAL)) {
        if (t->proc && t->proc->signal_wait_queue) {
            struct thread **pp = &t->proc->signal_wait_queue;
            while (*pp) {
                if (*pp == t) {
                    TRACE_THREAD("Removing thread %d from signal wait queue", t->tid);
                    *pp = (*pp)->next_sigwait;
                    break;
                }
                pp = &((*pp)->next_sigwait);  /* Correct dereferencing */
            }
        }
        t->wait_type &= ~WAIT_SIGNAL;
        t->next_sigwait = NULL;
        t->sig_wait_obj = NULL;
    }
    
    /* WAIT_JOIN */
    if ((wait_type_mask & WAIT_JOIN) && (t->wait_type & WAIT_JOIN) && t->join_wait_obj) {
        struct thread *target = (struct thread *)t->join_wait_obj;
        /* Clear the bidirectional relationship */
        if (target && target->magic == CTXT_MAGIC && target->joiner == t) {
            TRACE_THREAD("Clearing joiner reference for thread %d", target->tid);
            target->joiner = NULL;
        }
        t->wait_type &= ~WAIT_JOIN;
        t->join_wait_obj = NULL;
        t->join_retval = NULL;
    }
        
    /* WAIT_SLEEP */
    if ((wait_type_mask & WAIT_SLEEP) && (t->wait_type & WAIT_SLEEP)) {
        remove_from_sleep_queue(t->proc, t);
        t->wait_type &= ~WAIT_SLEEP;
        t->wakeup_time = 0;
    }
    
    spl(sr);
}

/**
 * O(1) check for ready queue membership using flag
 */
int is_in_ready_queue(struct thread *t) {
    TRACE_THREAD("Checking if thread %d is in ready queue", t->tid);
    if (!t || !t->proc)
        return 0;
    return t->in_ready_queue;  /* O(1) check using flag */
}

#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
/* Debug-only verification - walks queue to verify flag consistency */
int is_in_ready_queue_detailed(struct thread *t) {
    if (!t || !t->proc) return 0;
    
    struct thread *cur = t->proc->ready_queue;
    while (cur) {
        if (cur == t) {
            TRACE_THREAD("READY_Q: Thread %d verified in ready queue", t->tid);
            return 1;
        }
        cur = cur->next_ready;
    }
    TRACE_THREAD("READY_Q: Thread %d NOT in ready queue (DESYNC!)", t->tid);
    return 0;
}

int is_in_wait_queue(struct thread *head, struct thread *t) {
    while (head) {
        if (head == t) return 1;
        head = head->next_wait;
    }
    return 0;
}

int is_in_signal_wait_queue(struct proc *p, struct thread *t) {
    struct thread *curr = p->signal_wait_queue;
    while (curr) {
        if (curr == t) return 1;
        curr = curr->next_sigwait;
    }
    return 0;
}

/**
 * Check if a thread is in the sleep queue
 */
int is_in_sleep_queue(struct proc *p, struct thread *t) {
    if (!p || !t) return 0;
    
    struct thread *curr = p->sleep_queue;
    while (curr) {
        if (curr == t) {
            TRACE_THREAD("SLEEP_Q: Thread %d found in sleep queue", t->tid);
            return 1;
        }
        curr = curr->next_sleeping;
    }
    
    TRACE_THREAD("SLEEP_Q: Thread %d not found in sleep queue", t->tid);
    return 0;
}
#endif // THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE

/**
 * Find highest priority thread in a wait queue using bitmap optimization
 * 
 * @param queue The wait queue to search
 * @param prev_highest Pointer to store the previous thread of the highest priority thread
 * @return The highest priority thread, or NULL if none found
 */
struct thread *find_highest_priority_thread_in_queue(struct thread *queue, 
                                                     struct thread **prev_highest) {
    if (!queue) {
        TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Invalid queue");
        return NULL;
    }
    
    TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Finding highest priority thread in queue");
    /* Track highest priority thread found */
    struct thread *best_thread = NULL;
    struct thread *best_prev = NULL;
    int best_priority = -1;
    
    TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Checking first thread");
    struct thread *t = queue;
    struct thread *prev = NULL;
    
    /* Single pass: find highest priority thread directly */
    while (t) {
        TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Checking thread %d", t->tid);
        /* Validate thread */
        if (t->magic == CTXT_MAGIC && 
            !(t->state & THREAD_STATE_EXITED) && 
            t->priority < 17) {
            TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Found thread %d with priority %d", t->tid, t->priority);
            /* Track thread with highest priority (first wins ties) */
            if (t->priority > best_priority) {
                best_priority = t->priority;
                best_thread = t;
                best_prev = prev;
                TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Found thread %d with priority %d", t->tid, t->priority);
            }
        }
        prev = t;
        t = t->next_wait;
        TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Checking next thread");    
    }
    
    TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Found highest priority thread %d", best_thread ? best_thread->tid : -1);
    /* Return results */
    *prev_highest = best_prev;
    TRACE_THREAD("FIND_HIGHEST_PRIORITY_THREAD: Returning highest priority thread %d", best_thread ? best_thread->tid : -1);
    return best_thread;
}
