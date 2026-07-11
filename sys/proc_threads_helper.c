/**
 * @file proc_threads_helper.c
 * @brief Threading Utility Implementation
 * 
 * Implements core threading utilities including:
 *  - Priority bitmap management using fast lookup tables
 *  - Atomic thread state transitions
 *  - Priority scaling and boosting
 *  - Ready queue management
 *  - System tick access
 * 
 * Optimized for m68000 with inline functions, lookup tables, and
 * single-pass algorithms to minimize cycle count.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#include "proc_threads_helper.h"

/* Priority scaling lookup table - converts POSIX 0-99 to internal 0-16 range */
const unsigned char priority_scale_table[100] = {
    0,0,0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,4,4,4,4,4,4,5,5,5,5,5,5,6,6,6,6,6,7,7,7,7,7,7,8,8,8,8,8,8,9,9,9,9,9,9,10,10,10,10,10,11,11,11,11,11,11,12,12,12,12,12,12,13,13,13,13,13,14,14,14,14,14,14,15,15,15,15,15,15,16,16,16,16,16,16,16,16
};

/**
 * Scale thread priority from POSIX range (0-99) to internal bitmap range (0-16)
 * Uses precomputed lookup table for O(1) conversion with no multiplication
 * 
 * @param priority The priority value in POSIX range (0-99)
 * @return The scaled priority value in internal range (0-16)
 */
inline int scale_thread_priority(int priority) {
    /* Bounds check and table lookup */
    if ((unsigned)priority >= 100) return 16;
    int scaled_priority = priority_scale_table[priority];
    TRACE_THREAD("PRIORITY SCALE: POSIX %d -> Internal %d", priority, scaled_priority);
    return scaled_priority;
}

/**
* Update CPU time accounting for a thread
* Should be called when thread stops running (context switch, exit, block)
* Only counts time when thread was actually RUNNING and not waiting
*
* @param t Thread to update CPU time for
*/
void update_thread_cpu_time(struct thread *t) {
   unsigned long now, cpu_time_used;
   
   if (!t || t->magic != CTXT_MAGIC) {
       return;
   }
   
   now = get_system_ticks();
   
   /* Calculate how long thread has been scheduled */
   cpu_time_used = now - t->last_scheduled;
   
   /* Only count as CPU time if thread was RUNNING and not waiting
    * ANY wait type means thread wasn't actively using CPU */
   if (t->state == THREAD_STATE_RUNNING && t->wait_type == WAIT_NONE) {
       t->total_cpu_time += cpu_time_used;
       
       /* Prevent overflow: reset if exceeds ~3 hours at 200Hz (2^20 ticks = ~87 minutes)
        * This keeps values reasonable and prevents wraparound issues */
       if (t->total_cpu_time > (1UL << 20)) {
           t->total_cpu_time = (1UL << 8);  /* Reset to modest value (256 ticks) */
           TRACE_THREAD("CPU_TIME: Thread %d exceeded max CPU time, resetting", t->tid);
       }
   }
}

/**
* Reset CPU time tracking for a thread
* Used when thread priority is manually changed or for periodic resets
*
* @param t Thread to reset CPU time for
*/
void reset_thread_cpu_time(struct thread *t) {
   if (!t || t->magic != CTXT_MAGIC) {
       return;
   }
   
   t->total_cpu_time = 0;
}

/*
 * Calculate effective priority for SCHED_OTHER threads with aging
 * Uses bit shifts for fast division on m68k (no hardware divide)
 * 
 * @param t Thread to calculate priority for
 * @param now Current system tick count
 * @return Effective priority (0-16 range, higher is better)
 */
static inline int calculate_effective_priority(struct thread *t, unsigned long now) {
    unsigned long elapsed;
    int aging_bonus, cpu_penalty, effective;
    
    /* RT threads (SCHED_FIFO/SCHED_RR) always use base priority */
    if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
        return t->priority;
    }
    
    /* SCHED_OTHER threads get dynamic priority adjustment */
    
    /* Calculate aging bonus using fast bit shift instead of division
     * elapsed >> AGING_SHIFT is equivalent to elapsed / 128 
     * At 200Hz, 128 ticks = 640ms per bonus point */
    elapsed = now - t->last_scheduled;
    aging_bonus = (int)(elapsed >> AGING_SHIFT);
    if (aging_bonus > MAX_AGING_BONUS) {
        aging_bonus = MAX_AGING_BONUS;
    }
    
    /* Calculate CPU penalty using fast bit shift
     * total_cpu_time >> CPU_PENALTY_SHIFT is equivalent to total_cpu_time / 256
     * At 200Hz, 256 ticks = 1280ms per penalty point */
    cpu_penalty = (int)(t->total_cpu_time >> CPU_PENALTY_SHIFT);
    if (cpu_penalty > MAX_CPU_PENALTY) {
        cpu_penalty = MAX_CPU_PENALTY;
    }
    
    /* Calculate effective priority: base + aging - cpu_usage */
    effective = t->priority + aging_bonus - cpu_penalty;
    
    /* Clamp to valid range [0, 16] */
    if (effective < MIN_THREAD_PRIORITY) {
        effective = MIN_THREAD_PRIORITY;
    }
    if (effective > MAX_THREAD_PRIORITY) {
        effective = MAX_THREAD_PRIORITY;
    }
    
    return effective;
}

/**
 * Boost a thread's priority by a specified amount
 * 
 * @param t Thread to boost
 * @param boost_amount Amount to boost the priority by
 */
void boost_thread_priority(struct thread *t, int boost_amount) {
    if (!t || t->magic != CTXT_MAGIC) {
        return;
    }
    
    /* Save original priority if not already boosted */
    if (!t->priority_boost) {
        t->original_priority = t->priority;
    }
    
    /* Apply boost */
    t->priority = t->priority + boost_amount;
    /* Clamp to valid range */
    if (t->priority > MAX_THREAD_PRIORITY) t->priority = MAX_THREAD_PRIORITY;    
    t->priority_boost = 1;

    /* Reset CPU time when priority changes manually */
    reset_thread_cpu_time(t);

    TRACE_THREAD("Boosting priority of thread %d by %d", t->tid, boost_amount);

    return;
}

/**
 * Reset a thread's priority to its original value
 * 
 * @param t Thread to reset priority for
 */
void reset_thread_priority(struct thread *t) {
    if (!t || t->magic != CTXT_MAGIC || !t->priority_boost) {
        return;
    }
    
    TRACE_THREAD("Resetting priority of thread %d", t->tid);
    
    t->priority = t->original_priority;
    t->priority_boost = 0;

    /* Reset CPU time when boost expires */
    reset_thread_cpu_time(t);

    return;
}

/**
 * Helper function to get the highest priority thread from the ready queue
 *
* POSIX-compliant selection algorithm:
* 1. RT threads (SCHED_FIFO/SCHED_RR) always selected before SCHED_OTHER
* 2. Within RT threads: highest static priority wins, FIFO order for ties
* 3. SCHED_OTHER threads: dynamic priority with aging prevents starvation
* 4. Idle threads selected only if no other threads ready
*
* Performance: O(n) where n = ready queue length (typically 2-10 on Atari)
*/
struct thread *get_highest_priority_thread(struct proc *p)
{
    struct thread *t;
    struct thread *best_rt = NULL, *alt_rt = NULL;
    struct thread *best_other = NULL, *alt_other = NULL;
    struct thread *best_idle = NULL;
    short best_rt_pri = -1, best_other_pri = -1, best_idle_pri = -1;
    unsigned short effective_pri;
    unsigned long now;
    struct thread *current = CURTHREAD;

    if (!p || !p->ready_queue) {
        TRACE_THREAD("get_highest_priority_thread: Ready queue empty or Invalid process pointer");
        return NULL;
    }

    now = get_system_ticks();
    t = p->ready_queue;

    while (t) {
        if (t->magic != CTXT_MAGIC || (t->state & THREAD_STATE_EXITED)) {
            t = t->next_ready;
            continue;
        }

        /* ---- Idle threads ---- */
        if (t->is_idle) {
            if (t->priority > best_idle_pri) {
                best_idle = t;
                best_idle_pri = t->priority;
            }
        }

        /* ---- RT threads ---- */
        else if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
            if (t->priority > best_rt_pri) {
                alt_rt = best_rt;
                best_rt = t;
                best_rt_pri = t->priority;
            } else if (t->priority == best_rt_pri && t != best_rt) {
                alt_rt = t;
            }
        }

        /* ---- SCHED_OTHER ---- */
        else {
            effective_pri = calculate_effective_priority(t, now);

            if (effective_pri > best_other_pri) {
                alt_other = best_other;
                best_other = t;
                best_other_pri = effective_pri;
            } else if (effective_pri == best_other_pri && t != best_other) {
                alt_other = t;
            }
        }

        t = t->next_ready;
    }

    /* ---- POSIX selection with anti-self bias ---- */

    /* RT first */
    if (best_rt) {
        if (best_rt == current && alt_rt) {
            TRACE_THREAD("Selected alternate RT thread %d instead of current %d",
                         alt_rt->tid, current->tid);
            return alt_rt;
        }
        return best_rt;
    }

    /* Then SCHED_OTHER */
    if (best_other) {
        if (best_other == current && alt_other) {
            TRACE_THREAD("Selected alternate OTHER thread %d instead of current %d",
                         alt_other->tid, current->tid);
            return alt_other;
        }
        return best_other;
    }

    /* Finally idle */
    if (best_idle) {
        return best_idle;
    }

    return NULL;
}

struct thread *get_highest_priority_thread_excluding(struct proc *p, struct thread *exclude)
{
    struct thread *t;
    struct thread *best_rt = NULL, *best_other = NULL, *best_idle = NULL;
    short best_rt_pri = -1, best_other_pri = -1, best_idle_pri = -1;
    unsigned short effective_pri;
    unsigned long now;

    if (!p || !p->ready_queue) {
        return NULL;
    }

    now = get_system_ticks();
    t = p->ready_queue;

    while (t) {
        if (t == exclude ||
            t->magic != CTXT_MAGIC ||
            (t->state & THREAD_STATE_EXITED)) {
            t = t->next_ready;
            continue;
        }

        /* ---- Idle ---- */
        if (t->is_idle) {
            if (t->priority > best_idle_pri) {
                best_idle = t;
                best_idle_pri = t->priority;
            }
        }

        /* ---- RT ---- */
        else if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
            if (t->priority > best_rt_pri) {
                best_rt = t;
                best_rt_pri = t->priority;
            }
        }

        /* ---- SCHED_OTHER ---- */
        else {
            effective_pri = calculate_effective_priority(t, now);
            if (effective_pri > best_other_pri) {
                best_other = t;
                best_other_pri = effective_pri;
            }
        }

        t = t->next_ready;
    }

    /* POSIX order */
    if (best_rt)     return best_rt;
    if (best_other) return best_other;
    if (best_idle)  return best_idle;

    return NULL;
}

/**
 * Get the current system tick count.
 *
 * Returns the current system tick count for calculating time intervals.
 * The tick count is incremented by the system at 200 Hz.
 *
 * @return The current system tick count.
 */
inline unsigned long get_system_ticks(void) {
    return *((volatile unsigned long *)_hz_200);
}

/**
 * Make a process eligible for immediate selection as curproc
 * Increases chance that curproc will become equal to p
 */
void make_process_eligible(struct proc *p) {
    if (!p) return;
    
    register unsigned short sr = splhigh();
    
    /* If not in READY_Q or CURPROC_Q, add to READY_Q */
    if (p->wait_q != READY_Q && p->wait_q != CURPROC_Q) {
        TRACE_THREAD("make_process_eligible: Removing process %d from queue %d\n", p->pid, p->wait_q);
        if (p->wait_q) {
            rm_q(p->wait_q, p);
        }
        
        /* Add to front of READY_Q */
        p->wait_q = READY_Q;
        p->q_next = sysq[READY_Q].head;
        sysq[READY_Q].head = p;
        if (!p->q_next)
            sysq[READY_Q].tail = p;
        else
            p->q_next->q_prev = p;
        p->q_prev = NULL;
    }
    
    spl(sr);
}

/*
 * Atomic thread state change function
 * Ensures thread state transitions are atomic to prevent race conditions
 */
void proc_thread_state_change(struct thread *t, int new_state) {
    if (!t) {
        TRACE_THREAD("ERROR: Attemt to change thread state but no thread provided");
        return;
    }

    /* Skip if state already set */
    if (__builtin_expect(t->state == new_state, 0)) {
        return;
    }

    /* Check if thread is valid */
    if (t->magic != CTXT_MAGIC) {
        TRACE_THREAD("ERROR: Attempt to change state of invalid thread %d, magic=%lx", t->tid, t->magic);
        return;
    }

    /* Prevent transitions from EXITED state */
    if ((t->state == THREAD_STATE_EXITED) && !(new_state == THREAD_STATE_EXITED)) {
        TRACE_THREAD("ERROR : Attempt to change state of EXITED thread %d from %d to %d", t->tid, t->state, new_state);
        return;
    }

    // register unsigned short sr = splhigh();
    TRACE_THREAD("STATE CHANGED - thread pointer = %p, tid = %d, new_state=%d", t, t->tid, new_state);
    t->state = new_state;
    // spl(sr);
}

/*
 * Helper function to get remaining time on a timeout
 * WARNING: This walks the timeout list with interrupts disabled - 
 * consider using absolute wakeup times instead for hard real-time systems
 */
long timeout_remaining(TIMEOUT *t)
{
    if (!t)
        return 0;
        
    register unsigned short sr = splhigh();
    
    long remaining = 0;
    TIMEOUT *curr;
    
    for (curr = tlist; curr && curr != t; curr = curr->next) {
        remaining += curr->when;
    }
    
    if (curr == t) {
        remaining += curr->when;
    } else {
        /* Timeout not found in list */
        remaining = 0;
    }
    
    spl(sr);
    
    return remaining;
}

/*
 * Get the current thread's ID
 * Returns thread ID or -1 on error
 */
long sys_p_thread_getid(void) {
    struct thread *t = CURTHREAD;
    
    if (!t){
        TRACE_THREAD("sys_p_thread_getid: Warning - get tid called and no current thread");
        return -1;
    }
    TRACE_THREAD("sys_p_thread_getid: pid=%d, tid=%d", t->proc->pid, t->tid);
    return t->tid;
}

/*
 * Find a thread by TID in a process
 * @param p Process to search
 * @param tid Thread ID to find
 * @return Pointer to thread if found, NULL otherwise
 */
struct thread *proc_thread_find(struct proc *p, short tid) {
    if (!p || tid < 0) {
        return NULL;
    }

    register unsigned short sr = splhigh();
    struct thread *t = p->threads;
    
    while (t) {
        if (t->tid == tid && t->magic == CTXT_MAGIC) {
            spl(sr);
            return t;
        }
        t = t->next;
    }
    
    spl(sr);
    return NULL;
}
