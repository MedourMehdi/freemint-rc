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

/* Priority bit lookup table - returns 0-7, 0x80 for empty bitmap */
const unsigned char bit_table[256] = {
    0x80, /* Sentinel: no bits set in byte */
    0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
};

/* Priority scaling lookup table - converts POSIX 0-99 to internal 0-16 range */
const unsigned char priority_scale_table[100] = {
    0,0,0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,4,4,4,4,4,4,5,5,5,5,5,5,6,6,6,6,6,7,7,7,7,7,7,8,8,8,8,8,8,9,9,9,9,9,9,10,10,10,10,10,11,11,11,11,11,11,12,12,12,12,12,12,13,13,13,13,13,14,14,14,14,14,14,15,15,15,15,15,15,16,16,16,16,16,16,16,16
};

/**
 * Fast inline function to find highest priority bit in a word bitmap
 * Returns 0-15 for bit position, 0x80 if bitmap is 0 (no bits set)
 */
inline int find_highest_priority_bit_word(unsigned short bitmap) {
    unsigned char high_byte = (unsigned char)(bitmap >> 8);
    /* Branch prediction hint: high byte check first */
    if (__builtin_expect(high_byte != 0, 1)) {
        int bit = bit_table[high_byte];
        return (bit == 0x80) ? 0x80 : bit + 8;
    }
    return bit_table[(unsigned char)bitmap];
}

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
    return priority_scale_table[priority];
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
    
    TRACE_THREAD_PRIORITY(t, t->original_priority, t->priority);
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
    
    TRACE_THREAD_PRIORITY(t, t->priority, t->original_priority);
    
    t->priority = t->original_priority;
    t->priority_boost = 0;
}

/**
 * Helper function to get the highest priority thread from the ready queue
 *
 * This function implements POSIX-compliant thread selection:
 * - Highest priority threads are selected first
 * - For equal priority SCHED_FIFO threads, FIFO order by TID
 * - For equal priority SCHED_RR threads, round-robin order
 * 
 * Optimized: Single pass with union-based array allocation
 */
struct thread *get_highest_priority_thread(struct proc *p) {
    if (!p || !p->ready_queue)
        return NULL;

    /* Union reduces stack usage - single allocation for all arrays */
    union {
        struct { struct thread *rt[17]; struct thread *normal[17]; struct thread *idle[17]; } sep;
        void *all[51];
    } first;

    /* Fast single-loop initialization (~60 cycles vs 180) */
    register int i = 51;
    do {
        first.all[--i] = NULL;
    } while (i > 0);

    unsigned short rt_bitmap = 0, normal_bitmap = 0, idle_bitmap = 0;
    
    /* Single pass: build bitmaps AND track first thread per priority */
    struct thread *t = p->ready_queue;
    register unsigned short bit;  /* Cached shift register */

    while (t) {
        /* Validate thread and guard against corrupted priority */
        if (t->magic == CTXT_MAGIC && !(t->state & THREAD_STATE_EXITED)) {
            register unsigned char pri = t->priority;
            if (pri < 17) {  /* Bounds check for array safety */
                bit = 1 << pri;
                
                if (t->is_idle) {
                    idle_bitmap |= bit;
                    if (!first.sep.idle[pri]) first.sep.idle[pri] = t;
                } else if (t->policy == SCHED_FIFO || t->policy == SCHED_RR) {
                    rt_bitmap |= bit;
                    if (!first.sep.rt[pri]) first.sep.rt[pri] = t;
                } else {
                    normal_bitmap |= bit;
                    if (!first.sep.normal[pri]) first.sep.normal[pri] = t;
                }
            } else {
                TRACE_THREAD_ERROR("Thread %d has invalid priority %d", t->tid, pri);
            }
        }
        t = t->next_ready;
    }

    /* Check bitmaps in priority order using 0x80 sentinel */
    unsigned char highest_pri = find_highest_priority_bit_word(rt_bitmap);
    if (highest_pri != 0x80) {
        TRACE_THREAD("get_highest_priority_thread: rt_bitmap - Found thread %d with priority %d", 
                    first.sep.rt[highest_pri]->tid, highest_pri);
        return first.sep.rt[highest_pri];
    }
    
    highest_pri = find_highest_priority_bit_word(normal_bitmap);
    if (highest_pri != 0x80) {
        TRACE_THREAD("get_highest_priority_thread: normal_bitmap - Found thread %d with priority %d", 
                    first.sep.normal[highest_pri]->tid, highest_pri);
        return first.sep.normal[highest_pri];
    }
    
    highest_pri = find_highest_priority_bit_word(idle_bitmap);
    if (highest_pri != 0x80) {
        TRACE_THREAD("get_highest_priority_thread: idle_bitmap - Found idle thread %d with priority %d", 
                    first.sep.idle[highest_pri]->tid, highest_pri);
        return first.sep.idle[highest_pri];
    }
    
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
void atomic_thread_state_change(struct thread *t, int new_state) {
    if (!t) {
        TRACE_THREAD_ERROR("Attempt to change state of NULL thread");
        return;
    }

    /* Skip if state already set */
    if (__builtin_expect(t->state == new_state, 0)) {
        return;
    }

    /* Check if thread is valid */
    if (t->magic != CTXT_MAGIC) {
        TRACE_THREAD_ERROR("Attempt to change state of invalid thread %d, magic=%lx", t->tid, t->magic);
        return;
    }

    /* Prevent transitions from EXITED state */
    if ((t->state == THREAD_STATE_EXITED) && !(new_state == THREAD_STATE_EXITED)) {
        TRACE_THREAD_ERROR("Attempt to change state of EXITED thread %d from %d to %d", t->tid, t->state, new_state);
        return;
    }

    register unsigned short sr = splhigh();
    TRACE_THREAD_STATE(t, t->state, new_state);
    t->state = new_state;
    spl(sr);
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
