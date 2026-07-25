/******************************************************************************/
/* proc_threads_scheduler.c - Kernel Thread Scheduler Core (m68k Optimized)  */
/*                                                                            */
/* Implements preemptive thread scheduling with POSIX policies inside the    */
/* kernel. Handles context switching, priority inheritance, and thread exit  */
/* resource reclamation.                                                      */
/*                                                                            */
/* Optimization strategy:                                                     */
/*  - Minimize critical sections (splhigh/spl pairs)                          */
/*  - Prepare decisions outside locks where possible                          */
/*  - Reduce redundant memory operations and flag checks                      */
/*  - Cache frequently accessed values in registers                           */
/*  - Streamline control flow for better branch prediction                    */
/*                                                                            */
/* Author: Medour Mehdi                                                       */
/* Optimized for: Motorola 68000 architecture                                 */
/* Date: December 2025                                                        */
/******************************************************************************/

#include "arch/kernel.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_sleep_yield.h"
#include "proc_threads_policy.h"
#include "proc_threads_sync.h"
#include "proc_threads_signal.h"
#include "proc_threads_tsd.h"
#include "proc_threads_cleanup.h"
#include "proc_threads_cancel.h"

/* Scheduler state flags (single-byte for m68k efficiency) */
static volatile unsigned char timer_operation_locked = 0;
static volatile unsigned char thread_switch_in_progress = 0;

/* Structure to encapsulate thread switch context - reduces parameter passing */
struct thread_switch_context {
    struct thread *from;
    struct thread *to;
    struct proc *process;
    CONTEXT *to_ctx;
    unsigned long switch_time;
};

/* Structure to prepare scheduling decisions outside critical sections */
struct scheduling_decision {
    struct thread *current_thread;
    struct thread *next_thread;
    unsigned long decision_time;
    unsigned char should_switch;  /* 1-bit boolean */
};

/* Forward declarations */
static void reset_thread_switch_state(void);
static inline short should_schedule_thread(struct thread *current, 
                                           struct thread *next);
static void cancel_thread_timeouts(struct proc *p, struct thread *t);
static struct thread *find_next_thread_to_run(struct proc *p);
static void prepare_thread_switch(struct thread_switch_context *ctx);
static void execute_thread_switch(struct thread_switch_context *ctx);
static int prepare_scheduling_decision(struct proc *p, 
                                       struct scheduling_decision *decision);
static void execute_scheduling_decision(struct proc *p, 
                                        struct scheduling_decision *decision);

/******************************************************************************/
/* DEBUG: Ready Queue State Dump                                            */
/******************************************************************************/
#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
static void trace_ready_queue_dump(struct proc *p, const char *label) {
    struct thread *t;
    int count = 0;
    #ifdef DEBUG_THREAD
    unsigned long now = get_system_ticks();
    
    TRACE_THREAD("========== READY QUEUE DUMP: %s (time=%lu, current=%d) ==========",
                 label, now, p->current_thread ? p->current_thread->tid : -1);
    #endif
    if (!p || !p->ready_queue) {
        TRACE_THREAD("READY Q DUMP: Queue is EMPTY");
        goto done;
    }
    
    t = p->ready_queue;
    while (t && count < 50) {  // Limit to prevent lockup
        TRACE_THREAD("READY Q DUMP: [%02d] Thread %d (magic=%04lx) P=%d OP=%d EB=%d WT=%02x S=%d RQ=%d SQ=%d CPU=%lu",
                     count++,
                     t->tid, 
                     t->magic & 0xFFFF,
                     t->priority,
                     t->original_priority,
                     t->priority_boost,
                     t->wait_type,
                     t->state,
                     t->in_ready_queue,
                     t->in_sleep_queue,
                     t->total_cpu_time);
        
        // Check for corruption
        if (t->magic != CTXT_MAGIC) {
            TRACE_THREAD("READY Q DUMP: *** CORRUPT MAGIC on thread %d ***", t->tid);
            break;
        }
        if (t->next_ready && t->next_ready == t) {
            TRACE_THREAD("READY Q DUMP: *** SELF-LOOP on thread %d ***", t->tid);
            break;
        }
        
        t = t->next_ready;
    }
    
    if (count >= 50) {
        TRACE_THREAD("READY Q DUMP: *** QUEUE TOO LONG, TRUNCATED ***");
    }
    
done:
    TRACE_THREAD("========== END READY QUEUE DUMP (%d threads) ==========", count);
}
#else
#define trace_ready_queue_dump(p, label) do {} while(0)
#endif


/******************************************************************************/
/* SYSCALL Context Synchronization Helpers                                   */
/*                                                                            */
/* syscall.S only reads/writes p->ctxt[SYSCALL]. For threads with tid>0 we   */
/* maintain a per-thread shadow in t->ctxt[SYSCALL]. When a thread is        */
/* RUNNING, p->ctxt[SYSCALL] must mirror that thread's context.              */
/******************************************************************************/

static inline void sync_sys_from_proc(struct thread *t) {
    if (!t || !t->proc) return;
    
    TRACE_THREAD("SYNC PROC: Saving process context to thread %d", t->tid);
    memcpy(&t->ctxt[SYSCALL], &t->proc->ctxt[SYSCALL], sizeof(CONTEXT));
}

static inline void sync_sys_to_proc(struct thread *t) {
    if (!t || !t->proc) return;
    
    TRACE_THREAD("SYNC SYS: Mirroring syscall context for thread %d", t->tid);
    memcpy(&t->proc->ctxt[SYSCALL], &t->ctxt[SYSCALL], sizeof(CONTEXT));
}

/******************************************************************************/
/* thread_preempt_handler - Periodic preemption handler                      */
/*                                                                            */
/* Called by timer interrupt to implement preemptive multithreading.         */
/* Checks if current thread should be preempted and schedules another if so. */
/******************************************************************************/
void thread_preempt_handler(PROC *p, long arg) {

    struct thread *thread_arg = (struct thread *)arg;
    register unsigned short sr = splhigh();

    TRACE_THREAD("PREEMPT: Preemption handler invoked for process %d, current thread=%d, arg thread=%d", 
                 p->pid, 
                 p->current_thread ? p->current_thread->tid : -1, 
                 thread_arg ? thread_arg->tid : -1);
                 
    /* If not current process, reschedule the timeout */
    if (p != curproc) {
        spl(sr);
        TRACE_THREAD("PREEMPT: looking for process id %d but current is %d, rescheduling timeout", p->pid, curproc->pid);
        TRACE_THREAD("PREEMPT: Rescheduling preemption timer for process %d, prio is %d", p->pid, p->pri);
        reschedule_preemption_timer(p, (long)p->current_thread);
        return;
    }

    /* 
     * Check NULL current_thread early to prevent bus errors during cleanup 
     * P_FLAG_THREADED block preempt for forked process from thread env.
     */
    if (!p->current_thread 
        || !(p->p_flag & P_FLAG_THREADED)
    ) {
        spl(sr);
        TRACE_THREAD("PREEMPT: No current thread (process exiting), aborting");
        return;
    }

    /* Wake sleeping threads if any exist */
    if (p->sleep_queue) {
        check_and_wake_sleeping_threads(p);
    }

    /* Check for pending signals (regardless of whether we're current process) */
    if (p->current_thread->has_run && p->p_sigacts && 
        p->p_sigacts->thread_signals) {
        int sig = check_thread_signals(p->current_thread);
        if (sig > 0) {
            TRACE_THREAD("PREEMPT: Thread %d has pending signal %d / "
                        "Calling dispatch_thread_signals()", 
                        p->current_thread->tid, sig);
            dispatch_thread_signals(p->current_thread);
        }
    }
    
    /* Validate thread argument */
    if (thread_arg && (thread_arg->magic != CTXT_MAGIC || 
                       thread_arg->proc != p)) {
        TRACE_THREAD("PREEMPT: Invalid thread argument, using current thread");
        thread_arg = p->current_thread;
    }
    
    /* Protection against reentrance */
    if (p->p_thread_timer.in_handler > 0) {
        if (!p->p_thread_timer.enabled) {
            TRACE_THREAD("PREEMPT: Timer disabled, not rescheduling");
            spl(sr);
            return;
        }
        TRACE_THREAD("PREEMPT: Reentrance detected, rescheduling");
        /* Nested: bump counter, outer handler will reschedule */
        p->p_thread_timer.in_handler++;
        spl(sr);
        return;
    }
    
    /* Mark handler as active */
    p->p_thread_timer.in_handler = 1;  /* Outer: counter = 1 */
    p->p_thread_timer.timeout = NULL;
    spl(sr);

    TRACE_THREAD("PREEMPT: Current thread=%d, arg thread=%d",
                p->current_thread ? p->current_thread->tid : -1,
                thread_arg ? thread_arg->tid : -1);

    /* Run scheduler (may switch threads) */
    proc_thread_schedule();
    
    /* Use CURRENT thread, not cached -- may have exited during switch */
    TRACE_THREAD("PREEMPT: Rescheduling current thread %d",
                p->current_thread ? p->current_thread->tid : -1);
    sr = splhigh();
    p->p_thread_timer.in_handler--;  /* Only outer decrements */
    spl(sr);
    reschedule_preemption_timer(p, (long)p->current_thread);
}

/******************************************************************************/
/* proc_thread_schedule - Core scheduling algorithm                          */
/*                                                                            */
/* Selects next thread based on priority and policy, performs context switch */
/* if needed. Uses two-phase approach: prepare decision outside critical     */
/* section, then execute in minimal critical section.                        */
/******************************************************************************/
void proc_thread_schedule(void) {
    
    struct proc *p = get_curproc();
    struct scheduling_decision decision;

    if (thread_switch_in_progress) {
        TRACE_THREAD("SCHED: Nested call detected, aborting");
        return;
    }

    /* DEBUG: Dump queue state BEFORE scheduling */
    trace_ready_queue_dump(p, "PRE-SCHEDULE");

    /* 
     * Validate process has threads 
     * P_FLAG_THREADED block scheduling for forked process from thread env.
     */
    if (!p->threads 
        // || !(p->p_flag & P_FLAG_THREADED)
    ) {
        TRACE_THREAD("SCHED: Invalid current process, no threads to schedule on, pid %d, p_flag %lx", p->pid, p->p_flag);
        return;
    }

    /* Initialize decision structure (zeroed for safety) */
    decision.current_thread = NULL;
    decision.next_thread = NULL;
    decision.decision_time = 0;
    decision.should_switch = 0;

    /* Phase 1: Prepare scheduling decision (outside critical section) */
    if (!prepare_scheduling_decision(p, &decision)) {
        TRACE_THREAD("SCHED: No scheduling decision made, exiting");
        return;
    }

    /* Phase 2: Execute decision in minimal critical section */
    execute_scheduling_decision(p, &decision);
}

/******************************************************************************/
/* handle_thread_joining - Handle thread join during exit                    */
/*                                                                            */
/* Wakes up joiner thread and stores return value if thread is being joined. */
/******************************************************************************/
void handle_thread_joining(struct thread *current, void *retval) {
    struct thread *joiner;
    
    /* Quick validation */
    if (!current || !current->joiner || 
        current->joiner->magic != CTXT_MAGIC) {
        return;
    }
    
    joiner = current->joiner;
    
    TRACE_THREAD("EXIT: Thread %d is being joined by thread %d", 
                current->tid, joiner->tid);
    
    /* Check if joiner is actually waiting for this thread */
    if ((joiner->wait_type & WAIT_JOIN) && 
        joiner->join_wait_obj == current) {
        
        /* Clear wait state */
        joiner->wait_type &= ~WAIT_JOIN;
        joiner->join_wait_obj = NULL;
        
        /* Store return value in joiner's requested location */
        if (joiner->join_retval) {
            *(joiner->join_retval) = retval;
        }
        
        /* Mark as joined */
        current->joined = 1;
        
        /* Wake up joiner */
        proc_thread_state_change(joiner, THREAD_STATE_READY);
        add_to_ready_queue(joiner);
        
        TRACE_THREAD("EXIT: Woke up joining thread %d", joiner->tid);
    }
}

/******************************************************************************/
/* cancel_thread_timeouts - Cancel all timeouts for a thread                 */
/******************************************************************************/
static void cancel_thread_timeouts(struct proc *p, struct thread *t) {
    TIMEOUT *timelist, *next_timelist;
    
    if (!p || !t) {
        return;
    }
    
    /* Cancel sleep timeout specifically first */
    if (t->sleep_timeout) {
        TRACE_THREAD("EXIT: Cancelling sleep timeout for thread %d", t->tid);
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }
    
    /* Cancel all other timeouts associated with this thread */
    for (timelist = tlist; timelist; timelist = next_timelist) {
        next_timelist = timelist->next;
        if (timelist->proc == p && timelist->arg == (long)t) {
            TRACE_THREAD("EXIT: Cancelling timeout with thread %d as argument", 
                        t->tid);
            canceltimeout(timelist);
        }
    }
}

/******************************************************************************/
/* find_next_thread_to_run - Find next thread after current thread exits     */
/*                                                                            */
/* Search order:                                                              */
/* 1. Wake sleeping threads that should wake                                 */
/* 2. Check ready queue for highest priority thread                          */
/* 3. Fall back to thread0 if available                                      */
/* 4. Create idle thread if there are joinable threads waiting               */
/******************************************************************************/
static struct thread *find_next_thread_to_run(struct proc *p) {
    struct thread *next_thread = NULL;
    struct thread *t;
    unsigned long current_time;
    int woke_threads;
    int has_joinable_threads;
    
    if (!p) {
        return NULL;
    }
    
    /* If only one thread left, return main thread */
    if (p->num_threads == 1) {
        TRACE_THREAD("FIND NEXT THREAD: Only one thread left, returning main thread");
        return get_main_thread(p);
    }
    
    /* Step 1: Check sleep queue for threads that should wake up */
    if (p->sleep_queue) {
        current_time = get_system_ticks();
        
        TRACE_THREAD("FIND NEXT THREAD: Checking sleep queue at time %lu", current_time);
        
        woke_threads = wake_threads_by_time(p, current_time);
        
        if (woke_threads > 0) {
            TRACE_THREAD("FIND NEXT THREAD: Woke up %d threads from sleep queue", 
                        woke_threads);
        }
    }
    
    /* Step 2: Check ready queue for next thread */
    next_thread = get_highest_priority_thread(p);
    
    /* Validate thread from ready queue */
    while (next_thread && (next_thread->magic != CTXT_MAGIC || 
                          (next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("FIND NEXT THREAD: Skipping invalid thread %d in ready queue", 
                    next_thread->tid);
        remove_from_ready_queue(next_thread);
        next_thread = p->ready_queue;
    }
    
    if (next_thread) {
        TRACE_THREAD("FIND NEXT THREAD: Found next thread %d in ready queue", 
                    next_thread->tid);
        remove_from_ready_queue(next_thread);
        return next_thread;
    }
    
    /* Step 3: Try to find thread0 (if more than 1 thread exists) */
    if (p->num_threads > 1) {
        TRACE_THREAD("FIND NEXT THREAD: Looking for thread0 (num_threads=%d)", 
                    p->num_threads);
        
        for (t = p->threads; t; t = t->next) {
            if (t->tid == 0 && 
                t->magic == CTXT_MAGIC && 
                !(t->state & THREAD_STATE_EXITED) &&
                t->wait_type == WAIT_NONE) {
                
                next_thread = t;
                TRACE_THREAD("FIND NEXT THREAD: Found thread0 at %p, state=%d, "
                           "wait_type=%d", 
                           next_thread, next_thread->state, 
                           next_thread->wait_type);
                break;
            }
        }
    }
    
    /* Step 4: Create idle thread if joinable threads exist */
    if (!next_thread) {
        has_joinable_threads = 0;
        
        for (t = p->threads; t; t = t->next) {
            if (t->magic == CTXT_MAGIC && t->joiner && 
                t->joiner->magic == CTXT_MAGIC && 
                !(t->state & THREAD_STATE_EXITED)) {
                has_joinable_threads = 1;
                break;
            }
        }
        
        if (has_joinable_threads) {
            TRACE_THREAD("FIND NEXT THREAD: Creating idle thread for "
                        "joinable threads");
            next_thread = get_idle_thread(p);
        } else {
            TRACE_THREAD("FIND NEXT THREAD: No threads available");
        }
    }
    
    return next_thread;
}

/******************************************************************************/
/* cleanup_thread_resources - Clean up thread resources during exit          */
/*                                                                            */
/* Cleans up all thread-specific resources in proper order to avoid leaks    */
/* and race conditions. Frees memory if thread is detached or joined.        */
/******************************************************************************/
void cleanup_thread_resources(struct proc *p, struct thread *t, int tid) {
    struct thread **tp;
    int should_free;
    
    if (!p || !t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up resources: Invalid thread %d", tid);
        return;
    }
    
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up resources for thread %d", tid);

    /* Clean up subsystems in order */
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread sleep state");
    cleanup_thread_sleep_state(t);
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread tsd");
    cleanup_thread_tsd(t);    
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread cancellation");
    cleanup_thread_handlers(t);    
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread stack");
    cleanup_signal_stack(p, (long)t);
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread signals");
    cleanup_thread_signals(t);
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread cancellation");
    cleanup_thread_cancellation(t);
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleaning up thread timeouts");
    cancel_thread_timeouts(p, t);

    /* Clear thread signal state */
    t->t_sigpending = 0;
    THREAD_SIGMASK_SET(t, 0);
    t->t_sig_in_progress = 0;
    
    /* Determine if we should free resources */
    should_free = (t->detached || t->joined) && 
                  tid != 0 && 
                  !(t->joiner && t->joiner->magic == CTXT_MAGIC);
    
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Thread %d detached=%d, joined=%d, has_joiner=%d, "
                "should_free=%d", 
                tid, t->detached, t->joined, (t->joiner != NULL), should_free);
    
    /* Remove from thread list if freeing */
    if (should_free) {
        for (tp = &p->threads; *tp; tp = &(*tp)->next) {
            if (*tp == t) {
                *tp = t->next;
                break;
            }
        }
        TRACE_THREAD("CLEANUP THREAD RESOURCES: Removed thread %d from thread list", tid);
    }
    
    /* Clear current_thread pointer to prevent use-after-free */
    if (p->current_thread == t && t->tid != 0) {
        /* Redirect to thread 0, never NULL — sendsig needs a valid current_thread */
        struct thread *th;
        for (th = p->threads; th; th = th->next) {
            if (th->tid == 0 && th->magic == CTXT_MAGIC) {
                p->current_thread = th;
                break;
            }
        }
    }
    TRACE_THREAD("CLEANUP THREAD RESOURCES: Cleared current_thread pointer for thread %d", tid);
    
    /* Free resources if appropriate */
    if (should_free) {
        /* Free stack (except for thread0 which uses process stack) */
        if (t->stack && tid != 0) {
            TRACE_THREAD("CLEANUP THREAD RESOURCES: Freeing stack for thread %d", tid);
            kfree(t->stack);
            t->stack = NULL;
        }
        
        /* Clear magic BEFORE freeing to prevent use-after-free */
        t->magic = 0;
        
        kfree(t);
        TRACE_THREAD("CLEANUP THREAD RESOURCES: KFREE thread %d", tid);
    } else {
        TRACE_THREAD("CLEANUP THREAD RESOURCES: Thread %d not detached/joined or has joiner, or is the main thread (proc shadow), "
                    "keeping resources", tid);
    }
}

/******************************************************************************/
/* proc_thread_exit - Thread exit function                                   */
/*                                                                            */
/* Handles complete thread termination including cleanup handlers, TSD       */
/* destructors, joining, and resource cleanup. Switches to next thread.      */
/******************************************************************************/
void proc_thread_exit(void *retval, void *arg) {
    struct proc *p = curproc;
    struct thread *current;
    struct thread *next_thread;
    CONTEXT *to_ctx;
    static volatile unsigned char thread_exit_in_progress = 0;
    static volatile short exit_owner_tid = -1;
    short tid;
    int sig;

    /* Determine which thread is exiting */
    if (!arg) {
        current = p->current_thread;
        TRACE_THREAD("EXIT: Thread %d is exiting (EXIT THREAD)", 
                    current->tid);
    } else {
        current = (struct thread *)arg;
        TRACE_THREAD("EXIT: Thread %d is exiting (CANCEL THREAD)", 
                    current->tid);
    }

    if (!current) {
        TRACE_THREAD("EXIT ERROR: No current thread - cannot proceed");
        return;
    }

    tid = current->tid;
    TRACE_THREAD("EXIT: Thread %d is exiting with retval=%p", tid, retval);

    /* Protection against reentrance */
    if (thread_exit_in_progress && exit_owner_tid != current->tid) {
        TRACE_THREAD("EXIT: Thread exit already in progress by thread %d, "
                    "waiting", exit_owner_tid);
        proc_thread_exit(retval, NULL);
        return;
    }

    /* Check if thread already exited */
    if (current->magic != CTXT_MAGIC || 
        (current->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("WARNING: proc_thread_exit: Thread %d already exited "
                    "or freed (magic=%lx, state=%d)",
                    tid, current->magic, current->state);
        return;
    }
    
    /* Prevent thread0 from exiting while other threads exist */
    if (current->tid == 0) {
        if (p->num_threads > 1) {
            TRACE_THREAD("EXIT: Preventing thread0 exit while other threads "
                        "exist (num_threads=%d)", p->num_threads);
            return;
        }
        TRACE_THREAD("EXIT: Allowing thread0 to exit - no other threads remain");
    }

    /* Mark exit in progress */
    thread_exit_in_progress = 1;
    exit_owner_tid = tid;
    TRACE_THREAD("EXIT: Thread %d is beginning exit process", tid);

    /* Cancel timeouts and clear sleep state */
    TRACE_THREAD("EXIT: Cancelling timeouts for thread %d", tid);
    cancel_thread_timeouts(p, current);

    if (current->wait_type & WAIT_SLEEP) {
        current->wait_type &= ~WAIT_SLEEP;
        current->wakeup_time = 0;
        remove_from_sleep_queue(p, current);
    }

    /* Handle pending signals before exit */
    if (current->t_sigpending) {
        sig = check_thread_signals(current);
        if (sig && current->sig_handlers[sig].handler) {
            TRACE_THREAD("EXIT: Thread %d has pending signal %d, handling "
                        "before exit / Calling handle_thread_signal()", 
                        current->tid, sig);
            handle_thread_signal(current, sig);
        }
    }

    /* Run cleanup handlers and TSD destructors */
    TRACE_THREAD("EXIT: Running cleanup handlers for thread %d", tid);
    run_cleanup_handlers(current);
    
    TRACE_THREAD("EXIT: Running tsd destructors for thread %d", tid);
    run_tsd_destructors(current);

    /* Store return value */
    current->retval = retval;

    /* Handle thread joining */
    TRACE_THREAD("EXIT: Handling thread joining for thread %d", tid);
    handle_thread_joining(current, retval);

    /* Remove from all queues */
    TRACE_THREAD("EXIT: Removing thread %d from wait queues and ready queue", 
                tid);
    remove_thread_from_wait_queues(current);
    remove_from_ready_queue(current);

    /* Mark thread as exited */
    TRACE_THREAD("EXIT: Marking thread %d as exited", tid);
    proc_thread_state_change(current, THREAD_STATE_EXITED);

    /* Handle idle thread exit specially */
    if (current->is_idle) {
        struct thread *t;
        int has_joinable_threads = 0;
        
        TRACE_THREAD("EXIT: Idle thread %d is exiting", tid);
        
        for (t = p->threads; t; t = t->next) {
            if (t->magic == CTXT_MAGIC && t->joiner && 
                t->joiner->magic == CTXT_MAGIC) {
                has_joinable_threads = 1;
                break;
            }
        }
        
        if (!has_joinable_threads) {
            TRACE_THREAD("EXIT: Idle thread no longer needed, exiting");
        }
    } else if (tid > 0) {
        p->num_threads--;
    }

    TRACE_THREAD("EXIT: Thread %d exited, num_threads=%d", tid, p->num_threads);

    /* Update CPU time one final time before exit */
    update_thread_cpu_time(current);

    /* Handle timers when only one thread remains */
    if (p->num_threads == 1) {
        TRACE_THREAD("EXIT: Only one thread remaining, stopping all timers");
        if (p->p_thread_timer.enabled) {
            TRACE_THREAD("PTHREAD EXIT: Stopping thread timer for process %d", p->pid);
            thread_timer_stop(p);
        }
    }
    
    /* Find next thread to run */
    next_thread = find_next_thread_to_run(p);
    TRACE_THREAD("EXIT: Found next thread %d to run after exit", 
                next_thread ? next_thread->tid : -1);
    
    if (!next_thread && tid != 0) {
        TRACE_THREAD("EXIT: Selecting Thread 0 as next thread to run");
        next_thread = get_main_thread(p);
    }

    if(!next_thread) {
        TRACE_THREAD("EXIT: No threads to run");
    }

    /* Check cancellation for next thread */
    if (next_thread) {
        TRACE_THREAD("EXIT: Checking if next thread %d is cancellable", 
                    next_thread->tid);
        check_thread_cancellation(next_thread);
    }

    /* Prepare next thread for running */
    if (next_thread && 
        !(next_thread->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("EXIT: Will switch to thread %d", next_thread->tid);
        proc_thread_state_change(next_thread, THREAD_STATE_RUNNING);
    }

    /* Clean up thread resources */
    TRACE_THREAD("EXIT: Cleaning up resources for exiting thread %d", tid);
    TRACE_THREAD("EXIT: Asked to exit thread %d, CURRENT ctxt SR=%x, PC=%lx, "
                "USP=%lx, SSP=%lx", 
                current->tid, current->ctxt[CURRENT].sr, 
                current->ctxt[CURRENT].pc, current->ctxt[CURRENT].usp, 
                current->ctxt[CURRENT].ssp);
    TRACE_THREAD("EXIT: Asked to exit thread %d, SYSCALL ctxt SR=%x, PC=%lx, "
                "USP=%lx, SSP=%lx", 
                current->tid, current->ctxt[SYSCALL].sr, 
                current->ctxt[SYSCALL].pc, current->ctxt[SYSCALL].usp, 
                current->ctxt[SYSCALL].ssp);

    if (current->tid == 0) {
        sync_sys_from_proc(current);
    }

    cleanup_thread_resources(p, current, tid);
    TRACE_THREAD("Thread %d exited", tid);
    
    /* Clear exit in progress flag */
    thread_exit_in_progress = 0;
    exit_owner_tid = -1;

    /* Update current thread */
    if(next_thread) {
        TRACE_THREAD("EXIT: Switching to next thread %d", next_thread->tid);
        p->current_thread = next_thread;

        /* Switch to next thread (never returns) */
        thread_switch(NULL, next_thread);        
    }

    TRACE_THREAD("EXIT: There isn't next thread to switch to, exiting thread %d", tid);
    /* Sync syscall context and update current thread */
    p->current_thread = current;
    sync_sys_to_proc(current);
    to_ctx = get_thread_context(current);
    if ((to_ctx->sr & 0x2000) == 0) leave_kernel();
    change_context(to_ctx);
    
    /* Should NEVER reach here */
    TRACE_THREAD("CRITICAL ERROR: Returned from thread_switch after "
                "thread exit!");
}

/******************************************************************************/
/* should_schedule_thread - Determine if thread switch should occur          */
/*                                                                            */
/* Implements POSIX scheduling policy logic:                                 */
/* - SCHED_FIFO: Runs until blocked or preempted by higher priority          */
/* - SCHED_RR: Round-robin at same priority with timeslice                   */
/* - SCHED_OTHER: Standard timesharing with priority and timeslice           */
/******************************************************************************/
static inline short should_schedule_thread(struct thread *current, 
                                           struct thread *next) {
    unsigned long elapsed;
    
    /* Validate next thread */
    if (!next) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Invalid thread");
        return 0;
    }
    
    /* If no current thread, always schedule next */
    if (!current) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): No current "
                    "thread, scheduling next thread %d", next->tid);
        return 1;
    }

    /* If next thread has signal pending, always schedule next */
    if(next->t_sig_in_progress && next->has_run) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Next thread has "
                    "signal pending, scheduling next thread %d", next->tid);
        return 1;
    }

    /* Never switch to self */
    if (current == next) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread "
                    "is same as next thread %d, not switching", next->tid);
        return 0;
    }

    /* Special case: idle thread always preemptible by normal threads */
    if (current->is_idle && !next->is_idle) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Idle thread "
                    "preempted by normal thread %d", next->tid);
        return 1;
    }

    /* If current thread not running, always schedule next */
    if ((current->state != THREAD_STATE_RUNNING) || 
        (current->state & THREAD_STATE_BLOCKED)) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread "
                    "%d is not running, scheduling next thread %d",
                    current->tid, next->tid);
        return 1;
    }

    /* Calculate elapsed time once (optimize for m68k) */
    elapsed = get_system_ticks() - current->last_scheduled;

    /* PRIORITY CHECK - Higher priority always preempts */
    if (next->priority >= current->priority) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Higher priority "
                    "thread %d (pri %d%s) preempting thread %d (pri %d)",
                    next->tid, next->priority, 
                    next->priority_boost ? " boosted" : "",
                    current->tid, current->priority);
        return 1;
    }

    /* RT threads always preempt SCHED_OTHER threads */
    if ((next->policy == SCHED_FIFO || next->policy == SCHED_RR) &&
        current->policy == SCHED_OTHER) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): RT thread %d "
                    "preempting SCHED_OTHER thread %d",
                    next->tid, current->tid);
        return 1;
    }

    /* Check minimum timeslice for equal/lower priority */
    if (next->priority <= current->priority && 
        elapsed < current->proc->thread_min_timeslice) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread "
                    "%d hasn't used minimum timeslice (%lu < %d)",
                    current->tid, elapsed, current->proc->thread_min_timeslice);
        return 0;
    }

    /* Equal priority handling */
    if (next->priority == current->priority) {
        /* SCHED_FIFO threads continue running until preempted by higher priority */
        if (current->policy == SCHED_FIFO && next->policy == SCHED_FIFO) {
            return 0; /* Equal priority FIFO threads never preempt */
        }

        /* SCHED_RR and SCHED_OTHER use timeslice */
        if (elapsed >= current->timeslice) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Thread %d timeslice expired (%lu >= %d), switching to %d",
                        current->tid, elapsed, current->timeslice, next->tid);
            return 1;
        }

        /* For SCHED_RR at same priority, yield after timeslice */
        if (current->policy == SCHED_RR && elapsed >= current->timeslice) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): SCHED_RR thread %d yielding after timeslice", current->tid);
            return 1;
        }

        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Thread %d timeslice not expired (%lu < %d)",
                    current->tid, elapsed, current->timeslice);
        return 0;
    }

    /* Different scheduling policies */
    if(next->policy != current->policy) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Different scheduling policies between current thread %d and next thread %d",
                    current->tid, next->tid);
        return 1;
    }

    TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Lower priority thread %d cannot preempt current thread %d",
                next->tid, current->tid);
    /* Lower priority threads don't preempt higher priority ones */
    return 0;
}

/******************************************************************************/
/* thread_switch - Perform context switch between threads                    */
/*                                                                            */
/* Two-phase approach: prepare switch outside critical section, then execute */
/* in minimal critical section for better interrupt latency on m68k.         */
/******************************************************************************/
void thread_switch(struct thread *from, struct thread *to) {
    struct thread_switch_context ctx;
    CONTEXT *to_ctx;
    
    TRACE_THREAD("SWITCH (thread_switch): In function thread_switch");
    
    /* Fast validation first */
    if (!to) {
        TRACE_THREAD("SWITCH (thread_switch): Invalid destination thread pointer");
        return;
    }
    
    /* Special case: if from is NULL, just switch to destination */
    if (!from) {
        to_ctx = get_thread_context(to);
        
        TRACE_THREAD("SWITCH (thread_switch): Switching to thread %d (no source thread)", to->tid);

        reset_thread_priority(to);

        to->proc->current_thread = to;
        to->last_scheduled = get_system_ticks();

        sync_sys_to_proc(to);

        if ((to_ctx->sr & 0x2000) == 0) leave_kernel();
        change_context(to_ctx);
        TRACE_THREAD("SWITCH ERROR (thread_switch): Should not reach here");
        return;
    }
    
    /* Special case: switching to self with signal pending */
    if (from == to && to->t_sig_in_progress && to->has_run) {
        TRACE_THREAD("SWITCH (thread_switch): Self-switch with signal in progress for thread %d", to->tid);
        
        CONTEXT *sig_ctx = &to->sig_ctx;
        CONTEXT *saved_ctx = &to->saved_ctx;

        if((save_context(saved_ctx)==0) && to->tid != 0) {
            saved_ctx->regs[0] = 1; 
            if(CURTHREAD == to) {
                memcpy(saved_ctx, &to->proc->ctxt[SYSCALL], sizeof(CONTEXT));
            }
            if (to->proc->current_thread == to) {
                memcpy(&to->proc->ctxt[SYSCALL], sig_ctx, sizeof(CONTEXT));
            }
            to->last_scheduled = get_system_ticks();
            TRACE_THREAD("SWITCH (thread_switch): Saved signal context successfully for thread %d", to->tid);
            if((sig_ctx->sr & 0x2000) == 0) leave_kernel();
            change_context(sig_ctx);
            return;
        }
        
        TRACE_THREAD("SWITCH ERROR (thread_switch): Should not return from signal context!");
        return;
    }

    if (from == to) {
        TRACE_THREAD("SWITCH (thread_switch): Source and destination threads are the same");
        return;
    }
    
    /* Initialize context structure (zero for safety) */
    ctx.from = from;
    ctx.to = to;
    ctx.process = from->proc;
    ctx.to_ctx = NULL;
    ctx.switch_time = get_system_ticks();
    
    /* PREPARATION PHASE - Outside critical section */
    prepare_thread_switch(&ctx);
    
    /* Execute the switch if preparation was successful */
    if (ctx.to && ctx.to_ctx) {
        execute_thread_switch(&ctx);
    }
}

/******************************************************************************/
/* prepare_thread_switch - Prepare thread switch outside critical section    */
/******************************************************************************/
static void prepare_thread_switch(struct thread_switch_context *ctx) {
    TRACE_THREAD("SWITCH (prepare_thread_switch): Preparing switch from %d to %d", 
                ctx->from->tid, ctx->to->tid);
    
    /* Check magic numbers and states in one go */
    if (ctx->from->magic != CTXT_MAGIC || ctx->to->magic != CTXT_MAGIC ||
        (ctx->from->state & THREAD_STATE_EXITED) || (ctx->to->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("SWITCH  (prepare_thread_switch): Invalid thread magic or exited state: from=%d, to=%d", 
                    ctx->from->tid, ctx->to->tid);
        ctx->to = NULL;
        return;
    }
    
    /* Special handling for thread0 */
    if (ctx->from->tid == 0 && (ctx->from->state & THREAD_STATE_EXITED) && 
        ctx->from->proc->num_threads > 1) {
        TRACE_THREAD("SWITCH  (prepare_thread_switch): Preventing thread0 exit while other threads running");
        proc_thread_state_change(ctx->from, THREAD_STATE_READY);
        ctx->to = NULL;
        return;
    }
    
    /* Verify stack integrity */
    if (ctx->from->stack_magic != STACK_MAGIC || ctx->to->stack_magic != STACK_MAGIC) {
        TRACE_THREAD("SWITCH ERROR (prepare_thread_switch): Stack corruption detected!");
        ctx->to = NULL;
        return;
    }
    
    /* Get destination context once */
    ctx->to_ctx = get_thread_context(ctx->to);
    if (!ctx->to_ctx) {
        TRACE_THREAD("SWITCH (prepare_thread_switch): Failed to get context for thread %d", ctx->to->tid);
        ctx->to = NULL;
        return;
    }
}

/******************************************************************************/
/* execute_thread_switch - Execute prepared thread switch in critical section*/
/******************************************************************************/
static void execute_thread_switch(struct thread_switch_context *ctx) {
    unsigned long now;
    int sig;
    CONTEXT *from_ctx;

    register unsigned short sr = splhigh();

    /* Check if another switch is in progress */
    if (thread_switch_in_progress) {
        TRACE_THREAD("SWITCH (execute_thread_switch): Another switch in progress, aborting");
        spl(sr);
        return;
    }
    
    /* Set switch in progress */
    thread_switch_in_progress = 1;

    /* Update CPU time accounting */
    now = get_system_ticks();
    
    update_thread_cpu_time(ctx->from);

    TRACE_THREAD("SWITCH (execute_thread_switch): Thread %d to %d scheduled at %lu (was %lu)", 
                ctx->from->tid, ctx->to->tid, now, ctx->to->last_scheduled);

    /* Reset priority boost if needed */
    if ((ctx->from->priority_boost && 
         (now - ctx->from->last_scheduled) > ctx->from->proc->thread_min_timeslice) &&
         (ctx->from->wait_type == WAIT_NONE)) {
        TRACE_THREAD("SWITCH (execute_thread_switch): Resetting priority boost for thread %d", ctx->from->tid);
        reset_thread_priority(ctx->from);
    }
    
    /* Check for pending signals in destination thread */
    if (ctx->to->proc->p_sigacts && ctx->to->proc->p_sigacts->thread_signals && ctx->to->tid > 0 && ctx->to->has_run) {
        /* Only dispatch if there's actually a handler */
        int has_pending = 0;

        for (sig = 1; sig < NSIG; sig++) {
            if (THREAD_SIGPENDING(ctx->to) & (1UL << sig)) {
                /* Check thread handler first */
                if (ctx->to->sig_handlers[sig].handler) {
                    TRACE_THREAD("SWITCH (execute_thread_switch): Thread %d has handler for pending signal %d", 
                                ctx->to->tid, sig);
                    has_pending = 1;
                    break;
                }
                /* Check process handler */
                struct sigaction *sa = &SIGACTION(ctx->to->proc, sig);
                if (sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN) {
                    TRACE_THREAD("SWITCH (execute_thread_switch): Process %d has handler for pending signal %d", 
                                ctx->to->proc->pid, sig);
                    has_pending = 1;
                    break;
                }
            }
        }
        
        if (has_pending) {
            spl(sr);
            TRACE_THREAD("SWITCH (execute_thread_switch): Dispatching signals for thread %d (has handler)", 
                        ctx->to->tid);
            dispatch_thread_signals(ctx->to);
            TRACE_THREAD("SWITCH (execute_thread_switch): Returned from dispatch_thread_signals for thread %d, "
                        "pending=%d, in_progress=%d", 
                        ctx->to->tid, ctx->to->t_sigpending, 
                        ctx->to->t_sig_in_progress);
            sr = splhigh();
            /* Refresh context after signal dispatch */
            if (ctx->to->t_sig_in_progress) {
                TRACE_THREAD("SWITCH (execute_thread_switch): Signal handler active, switching to signal context");
                ctx->to_ctx = get_thread_context(ctx->to);
            }
        } else {
            TRACE_THREAD("SWITCH (execute_thread_switch): Thread %d has pending signals but no handlers yet", 
                        ctx->to->tid);
        }
    }
    
    /* Handle context switch based on thread state */
    if (((ctx->from->wait_type & WAIT_SLEEP) || 
        (ctx->from->wait_type & WAIT_JOIN) || 
        (ctx->from->wait_type & WAIT_SEMAPHORE) || 
        (ctx->from->state & THREAD_STATE_EXITED)) ){
        
        // if( (ctx->from->tid == 0) || !(ctx->from->wait_type & WAIT_SEMAPHORE) )
        sync_sys_from_proc(ctx->from);

        TRACE_THREAD("SWITCH (execute_thread_switch): Thread %d is sleeping/joining/exited or has semaphore, skipping save", 
                    ctx->from->tid);

        proc_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
        reset_thread_priority(ctx->to);
        reset_thread_switch_state();

        TRACE_THREAD("SWITCH (execute_thread_switch): Switched to context for thread %d, ssp=%lx, "
                    "usp=%lx, pc=%lx, sr=%x", 
                    ctx->to->tid, ctx->to_ctx->ssp, 
                    ctx->to_ctx->usp, ctx->to_ctx->pc, ctx->to_ctx->sr);

        ctx->from->proc->current_thread = ctx->to;
        ctx->to->last_scheduled = get_system_ticks();

        sync_sys_to_proc(ctx->to);

        spl(sr);

        if ((ctx->to_ctx->sr & 0x2000) == 0) leave_kernel();
        change_context(ctx->to_ctx);
        
        TRACE_THREAD("SWITCH ERROR (execute_thread_switch): Should not reach here!");
    } else {

        /* Thread is running - save context before switching */
        if (ctx->from->wait_type == WAIT_NONE) {
            proc_thread_state_change(ctx->from, THREAD_STATE_READY);
        }

        from_ctx = get_thread_context(ctx->from);

        TRACE_THREAD("SWITCH (execute_thread_switch): Thread %d context - SSP=%lx, USP=%lx, PC=%lx", 
                    ctx->from->tid, from_ctx->ssp, from_ctx->usp, from_ctx->pc);

        if (save_context(from_ctx) == 0) {
            /* Context is NOW saved — safe to make from thread visible to scheduler */
            from_ctx->regs[0] = 1;

            sync_sys_from_proc(ctx->from);

            /* FIX: enqueue from-thread only after save_context() has captured its
            * state. Before this point, from_ctx->pc/sp are stale. */
            if (ctx->from->wait_type == WAIT_NONE &&
                !(ctx->from->state & THREAD_STATE_EXITED)) {
                add_to_ready_queue(ctx->from);
            }

            TRACE_THREAD("SWITCH (execute_thread_switch): Saved context for thread %d, "
                        "ssp=%lx, usp=%lx, pc=%lx, sr=%x, to=%d",
                        ctx->from->tid, from_ctx->ssp, from_ctx->usp,
                        from_ctx->pc, from_ctx->sr, ctx->to->tid);

            proc_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
            reset_thread_priority(ctx->to);
            reset_thread_switch_state();

            ctx->from->proc->current_thread = ctx->to;
            ctx->to->last_scheduled = get_system_ticks();

            sync_sys_to_proc(ctx->to);

            spl(sr);

            if ((ctx->to_ctx->sr & 0x2000) == 0) leave_kernel();
            change_context(ctx->to_ctx);

            TRACE_THREAD("SWITCH ERROR (execute_thread_switch): Should not reach here!");
        }

        /* RETURN PATH: thread resumed after context switch */
        reset_thread_switch_state();

        TRACE_THREAD("SWITCH (execute_thread_switch): Changed context after save for thread %d, "
                    "IN_DOS=%x, IN_KERNEL=%x",
                    CURTHREAD->tid, CURTHREAD->proc->in_dos, in_kernel);
    }
}

/******************************************************************************/
/* prepare_scheduling_decision - Prepare scheduling decision outside lock    */
/******************************************************************************/
static int prepare_scheduling_decision(struct proc *p, 
                                      struct scheduling_decision *decision) {
    struct thread *thread0;
    
    decision->current_thread = p->current_thread;
    decision->decision_time = get_system_ticks();
    
    /* Check and wake sleeping threads if needed */
    if (!decision->current_thread || 
        (decision->current_thread->last_scheduled + time_slice) < decision->decision_time) {
        check_and_wake_sleeping_threads(p);
    }

    TRACE_THREAD("SCHED (prepare_scheduling_decision): Current thread id=%d state=%d wait_type=%02x", 
                decision->current_thread->tid,
                decision->current_thread->state, 
                decision->current_thread->wait_type);
    
    if (decision->current_thread->wait_type) {
        TRACE_THREAD("SCHED (prepare_scheduling_decision): Thread %d is waiting on: %s", 
                     decision->current_thread->tid,
                     (decision->current_thread->wait_type & WAIT_MUTEX) ? "MUTEX" :
                     (decision->current_thread->wait_type & WAIT_CONDVAR) ? "CONDVAR" :
                     (decision->current_thread->wait_type & WAIT_SIGNAL) ? "SIGNAL" :
                     (decision->current_thread->wait_type & WAIT_SLEEP) ? "SLEEP" :
                     (decision->current_thread->wait_type & WAIT_CONDVAR) ? "CONDVAR" :
                     (decision->current_thread->wait_type & WAIT_SEMAPHORE) ? "SEMAPHORE" :
                     (decision->current_thread->wait_type & WAIT_JOIN) ? "JOIN" : "UNKNOWN");
    }

    if (decision->current_thread->t_sig_in_progress && 
        !(decision->current_thread->state & THREAD_STATE_BLOCKED)) {
        TRACE_THREAD("SCHED (prepare_scheduling_decision): Thread %d has signal pending", 
                        decision->current_thread->tid);
        decision->next_thread = decision->current_thread;
    } else {
        /* Get highest priority thread from ready queue */
        TRACE_THREAD("SCHED (prepare_scheduling_decision): Getting highest priority thread");
        decision->next_thread = get_highest_priority_thread(p);
    }

    TRACE_THREAD("SCHED (prepare_scheduling_decision): Next thread id=%d state=%d wait_type=%02x", 
                decision->next_thread ? decision->next_thread->tid : -1,
                decision->next_thread ? decision->next_thread->state : -1, 
                decision->next_thread ? decision->next_thread->wait_type : 0);
    /* Validate next thread */
    if (decision->next_thread && (decision->next_thread->magic != CTXT_MAGIC || 
                                 (decision->next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("SCHED (prepare_scheduling_decision): Next thread %d is invalid or exited, removing from ready queue", 
                    decision->next_thread->tid);
        remove_from_ready_queue(decision->next_thread);
        decision->next_thread = NULL;
    }
    
    /* Handle case where no next thread is found */
    if (!decision->next_thread) {
        /* Try to find thread0 first */
        thread0 = get_main_thread(p);
        
        if (thread0 && !(thread0->state & THREAD_STATE_EXITED) && 
            (thread0->wait_type == WAIT_NONE)) {
            decision->next_thread = thread0;
            TRACE_THREAD("SCHED (prepare_scheduling_decision): Falling back to thread0");
        } else if (decision->current_thread && 
                   !(decision->current_thread->state & THREAD_STATE_BLOCKED) && 
                   !decision->current_thread->is_idle) {
            TRACE_THREAD("SCHED (prepare_scheduling_decision): Continuing with current thread %d",  
                        decision->current_thread->tid);
            return 0; /* No switch needed */
        } else if (p->num_threads > 1 && thread0 && 
                   (thread0->wait_type & WAIT_JOIN)) {
            TRACE_THREAD("SCHED (prepare_scheduling_decision): No threads available, falling back to idle thread");
            decision->next_thread = get_idle_thread(p);
        } else {
            TRACE_THREAD("SCHED (prepare_scheduling_decision): No threads available");
            return 0;
        }
    }
    
    /* Check if we should schedule next thread */
    // decision->should_switch = should_schedule_thread(decision->current_thread,
    //                                                   decision->next_thread);
    decision->should_switch = 1;

    return decision->should_switch;
}

/******************************************************************************/
/* execute_scheduling_decision - Execute prepared scheduling decision        */
/******************************************************************************/
static void execute_scheduling_decision(struct proc *p,
                                       struct scheduling_decision *decision) {
    struct mutex *m;

    if (!decision->should_switch || !decision->next_thread) {
        TRACE_THREAD("SCHED (execute_scheduling_decision): No switch needed");
        return;
    }

    TRACE_THREAD("SCHED (execute_scheduling_decision): Executing switch from %d to %d",
                decision->current_thread ? decision->current_thread->tid : -1,
                decision->next_thread->tid);

    #if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
    trace_ready_queue_dump(p, "PRE-SWITCH");
    #endif
    
    /* Remove next from ready queue if it's there */
    if (is_in_ready_queue(decision->next_thread)) {
        remove_from_ready_queue(decision->next_thread);
    }

    /* Update thread states and prepare for switch */
    if (decision->current_thread) {
        update_thread_timeslice(decision->current_thread);

        if (decision->current_thread->state == THREAD_STATE_RUNNING) {
            update_thread_cpu_time(decision->current_thread);

            if (decision->current_thread->wait_type != WAIT_NONE) {
                proc_thread_state_change(decision->current_thread,
                                         THREAD_STATE_BLOCKED);

                /* Priority inheritance for mutexes */
                if ((decision->current_thread->wait_type & WAIT_MUTEX) &&
                    decision->current_thread->mutex_wait_obj) {

                    m = (struct mutex*)decision->current_thread->mutex_wait_obj;

                    if (m->owner &&
                        m->owner->priority < decision->current_thread->priority) {

                        boost_thread_priority(m->owner,
                                            decision->current_thread->priority -
                                            m->owner->priority);

                        if (m->owner->state == THREAD_STATE_READY) {
                            remove_from_ready_queue(m->owner);
                            add_to_ready_queue(m->owner);
                        }
                    }
                }
            } else {
                /* FIX: do NOT add current_thread back to the ready queue here.
                 * The thread is still RUNNING — its context hasn't been saved yet.
                 * Adding it now lets the scheduler pick it up before save_context()
                 * captures the new PC/SP, causing change_context() to resume from
                 * stale state on the next switch.
                 *
                 * The re-enqueue must happen inside execute_thread_switch() on
                 * the save_context() wakeup path — i.e. after the context is
                 * actually saved — which is what thread_switch() already does
                 * via the THREAD_STATE_READY transition inside save_context==0
                 * branch. So just mark it READY here and let thread_switch handle
                 * the queue insertion. */
                proc_thread_state_change(decision->current_thread,
                                         THREAD_STATE_READY);
                /* do NOT call add_to_ready_queue() here */
            }
        }
    }

    proc_thread_state_change(decision->next_thread, THREAD_STATE_RUNNING);

    thread_switch(decision->current_thread, decision->next_thread);
}

/******************************************************************************/
/* reschedule_preemption_timer - Reschedule the preemption timer             */
/******************************************************************************/
void reschedule_preemption_timer(PROC *p, long arg) {
    struct thread *t;
    
    if (!p) {
        TRACE_THREAD("SCHED_TIMER: Invalid process reference");
        return;
    }

    /* Cancel existing timeout first */
    if (p->p_thread_timer.timeout) {
        canceltimeout(p->p_thread_timer.timeout);
        p->p_thread_timer.timeout = NULL;
    }

    t = (struct thread *)arg;
    
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, 
                                          thread_preempt_handler);
    
    if (p->p_thread_timer.timeout) {
        p->p_thread_timer.timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SCHED_TIMER: Failed to reschedule preemption timer for "
                    "process %d", p->pid);
    }
}

/******************************************************************************/
/* reset_thread_switch_state - Reset thread switch state                     */
/******************************************************************************/
static void reset_thread_switch_state(void) {
    register unsigned short sr = splhigh();
    thread_switch_in_progress = 0;
    spl(sr);
}

/******************************************************************************/
/* thread_timer_start - Start thread timer for process                       */
/******************************************************************************/
void thread_timer_start(struct proc *p) {
    register unsigned short sr;
    unsigned char retry_count = 0;
    
    TRACE_THREAD("TIMER: thread_timer_start called for process %d", p->pid);
    
    if (!p) {
        return;
    }

    /* Try to acquire timer operation lock with timeout */
    while (1) {
        if (!timer_operation_locked) {
            TRACE_THREAD("TIMER START: Acquired timer operation lock");
            sr = splhigh();
            timer_operation_locked = 1;
            spl(sr);
            break;
        }
        
        /* Give up after 10 retries */
        if (retry_count++ >= 10) {
            TRACE_THREAD("TIMER WARNING: Failed to acquire timer operation lock "
                        "after 10 retries");
            return;
        }
    }

    /* CRITICAL SECTION */
    TRACE_THREAD("TIMER: Starting thread timer for process %d", p->pid);
    
    /* If timer already enabled, don't add another timeout */
    if (p->p_thread_timer.enabled && p->p_thread_timer.timeout) {
        TRACE_THREAD("TIMER: Timer already enabled, not adding another timeout");
        goto cleanup;
    }
    
    /* Create the timeout */
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, 
                                          thread_preempt_handler);
    if (!p->p_thread_timer.timeout) {
        TRACE_THREAD("TIMER ERROR: Failed to create timeout");
        goto cleanup;
    }

    /* Set the timeout argument to current thread */
    p->p_thread_timer.timeout->arg = (long)p->current_thread;

    /* Enable timer */
    p->p_thread_timer.enabled = 1;
    p->p_thread_timer.in_handler = 0;

    TRACE_THREAD("TIMER: Thread timer started for process %d with interval %dms", 
                p->pid, p->thread_preempt_interval);
    
cleanup:
    /* Always release lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    TRACE_THREAD("TIMER: Timer operation lock released");
}

/******************************************************************************/
/* thread_timer_stop - Stop thread timer                                     */
/******************************************************************************/
void thread_timer_stop(PROC *p) {
    register unsigned short sr;
    unsigned char retry_count = 0;

    if (!p) {
        return;
    }

    /* Try to acquire timer operation lock with timeout */
    while (1) {
        if (!timer_operation_locked) {
            TRACE_THREAD("TIMER STOP: Acquired timer operation lock");
            sr = splhigh();
            timer_operation_locked = 1;
            spl(sr);
            break;
        }

        /* Give up after 10 retries */
        if (++retry_count > 10) {
            TRACE_THREAD("TIMER WARNING: Failed to acquire timer operation lock "
                        "after 10 retries");
            return;
        }
    }

    /* CRITICAL SECTION */
    TRACE_THREAD("TIMER: Stopping thread timer for process %d", p->pid);
    
    /* Disable timer if only one thread remains */
    if (p->num_threads <= 1) {
        TRACE_THREAD("TIMER: Disabling timer for process %d (num_threads=%d)", 
                    p->pid, p->num_threads);
        p->p_thread_timer.enabled = 0;
    }
    
    /* Cancel the timeout */
    if (p->p_thread_timer.timeout) {
        canceltimeout(p->p_thread_timer.timeout);
        TRACE_THREAD("TIMER: Cancelled timeout");
    }
    
    /* Always release lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    
    TRACE_THREAD("TIMER: Timer operation lock released");
}