/**
 * @file proc_threads_scheduler.c
 * @brief Kernel Thread Scheduler Core
 * 
 * Implements preemptive thread scheduling with POSIX policies inside the kernel.
 * Handles context switching, priority inheritance, and thread exit resource reclamation.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

 /**
 * Thread Scheduler Core
 * 
 * Implements preemptive thread scheduling with POSIX-compliant policies 
 * (FIFO, RR, OTHER). Features timeslice management, priority inheritance, 
 * and robust thread exit handling with resource cleanup.
 */

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
#include "arch/kernel.h"

static void reset_thread_switch_state(void);
static void thread_switch_timeout_handler(PROC *p, long arg);

/* Thread scheduling helper functions */
static inline short should_schedule_thread(struct thread *current, struct thread *next);
// static void thread_switch(struct thread *from, struct thread *to);

/* Thread exit helper functions */
static void cancel_thread_timeouts(struct proc *p, struct thread *t);
static struct thread *find_next_thread_to_run(struct proc *p);

#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
static inline short fix_orphaned_thread(struct thread *t, struct proc *p);
static void check_orphaned_threads(struct proc *p);
#endif

/* Mutex for timer operations */
static short timer_operation_locked = 0;
static short thread_switch_in_progress = 0;
static TIMEOUT *thread_switch_timeout = NULL;

/* Structure to encapsulate thread switch context and reduce parameter passing */
struct thread_switch_context {
    struct thread *from, *to;
    struct proc *process;
    CONTEXT *to_ctx;
    unsigned long switch_time;
};

/* Structure to prepare scheduling decisions outside critical sections */
struct scheduling_decision {
    struct thread *current_thread, *next_thread;
    unsigned long decision_time;
    unsigned char should_switch : 1; // 1-bit boolean
};


/* Forward declarations for new functions */
static void prepare_thread_switch(struct thread_switch_context *ctx);
static void execute_thread_switch(struct thread_switch_context *ctx);
static int prepare_scheduling_decision(struct proc *p, struct scheduling_decision *decision);
static void execute_scheduling_decision(struct proc *p, struct scheduling_decision *decision);

/**
 * Thread preemption handler
 * 
 * This function is called periodically to implement preemptive multithreading.
 * It checks if the current thread should be preempted and schedules another thread if needed.
 */
void thread_preempt_handler(PROC *p, long arg) {
    register unsigned short sr;
    struct thread *thread_arg = (struct thread *)arg;
    
    if (!p) {
        TRACE_THREAD("PREEMPT: Invalid process pointer");
        return;
    }

    // If not current process, reschedule the timeout
    if (p != curproc) {
        TRACE_THREAD("PREEMPT: Not current process, rescheduling timeout");
        reschedule_preemption_timer(p, (long)p->current_thread);
        return;
    }
    // Check for sleeping threads first
    if (p->sleep_queue) {
        TRACE_THREAD("PREEMPT: Checking sleep queue for process %d", p->pid);
        check_and_wake_sleeping_threads(p);
    }

    TRACE_THREAD("PREEMPT: Timer fired for process %d", p->pid);
    // Validate thread argument once
    if (thread_arg && (thread_arg->magic != CTXT_MAGIC || thread_arg->proc != p)) {
        TRACE_THREAD("PREEMPT: Invalid thread argument, using current thread");
        thread_arg = p->current_thread;
    }
    
    // Protection against reentrance
    if (p->p_thread_timer.in_handler) {
        if (!p->p_thread_timer.enabled) {
            TRACE_THREAD("PREEMPT: Timer disabled, not rescheduling");
            return;
        }
        TRACE_THREAD("PREEMPT: Already in handler, rescheduling");
        reschedule_preemption_timer(p, (long)p->current_thread);
        return;
    }
    
    sr = splhigh();
    p->p_thread_timer.in_handler = 1;
    p->p_thread_timer.timeout = NULL;
    struct thread *curr_thread = p->current_thread;

    TRACE_THREAD("PREEMPT: Current thread is %d, arg thread is %d", 
                curr_thread ? curr_thread->tid : -1, 
                thread_arg ? thread_arg->tid : -1);

    spl(sr);
    proc_thread_schedule();
    TRACE_THREAD("PREEMPT: No switch needed, rescheduling current thread %d", curr_thread->tid);
    // No switch needed, reschedule timer
    reschedule_preemption_timer(p, (long)curr_thread);
    
}

/**
 * Schedule a new thread to run
 * 
 * This function implements the core scheduling algorithm for threads.
 * It selects the next thread to run based on priority and scheduling policy,
 * and performs the context switch if needed.
 */
void proc_thread_schedule(void) {
    struct proc *p = get_curproc();
    
    if (!p->threads) {
        TRACE_THREAD("SCHED: Invalid current process, no threads to schedule");
        return;
    }

    TRACE_THREAD("SCHED: Entered scheduler");
    
    // Create a scheduling decision structure
    struct scheduling_decision decision = {0};
    
    // Prepare the scheduling decision outside critical section
    if (!prepare_scheduling_decision(p, &decision)) {
        TRACE_THREAD("SCHED: No scheduling decision made, returning");
        return;
    }
    
    // Execute the scheduling decision in minimal critical section
    execute_scheduling_decision(p, &decision);
    TRACE_THREAD("SCHED: Should not reach here, scheduling decision executed");
    return;
}

/**
 * Handle thread joining during thread exit
 */
void handle_thread_joining(struct thread *current, void *retval) {
    if (!current || !current->joiner || current->joiner->magic != CTXT_MAGIC) {
        return;
    }
    
    struct thread *joiner = current->joiner;
    TRACE_THREAD("EXIT: Thread %d is being joined by thread %d", 
                current->tid, joiner->tid);
    
    // If joiner is waiting for this thread
    if ((joiner->wait_type & WAIT_JOIN) && joiner->join_wait_obj == current) {
        // Wake up the joining thread
        joiner->wait_type &= ~WAIT_JOIN;
        joiner->join_wait_obj = NULL;
        
        // Store return value directly in joiner's requested location
        if (joiner->join_retval) {
            *(joiner->join_retval) = retval;
        }
        
        // Mark as joined
        current->joined = 1;
        
        // Wake up joiner
        atomic_thread_state_change(joiner, THREAD_STATE_READY);
        add_to_ready_queue(joiner);
        TRACE_THREAD("EXIT: Woke up joining thread %d", joiner->tid);
    }
}

/**
 * Cancel all timeouts associated with a thread
 */
static void cancel_thread_timeouts(struct proc *p, struct thread *t) {
    if (!p || !t) {
        return;
    }
    // Cancel sleep timeout specifically first
    if (t->sleep_timeout) {
        TRACE_THREAD("EXIT: Cancelling sleep timeout for thread %d", t->tid);
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }    
    TIMEOUT *timelist, *next_timelist;
    for (timelist = tlist; timelist; timelist = next_timelist) {
        next_timelist = timelist->next;
        if (timelist->proc == p && timelist->arg == (long)t) {
            TRACE_THREAD("EXIT: Cancelling timeout with thread %d as argument", t->tid);
            canceltimeout(timelist);
        }
    }
}

/**
 * Find the next thread to run after a thread exits
 */
static struct thread *find_next_thread_to_run(struct proc *p) {
    struct thread *next_thread = NULL;
    
    if (!p) {
        return NULL;
    }
    
    // If only one thread left, return main thread
    if (p->num_threads == 1) {
        TRACE_THREAD("EXIT: Only one thread left, returning main thread");
        return get_main_thread(p);
    }
    
    // STEP 1: First check the sleep queue for threads that should wake up
    if (p->sleep_queue) {
        unsigned long current_time = get_system_ticks();
        int woke_threads;
        
        TRACE_THREAD("EXIT: Checking sleep queue at time %lu", current_time);
        
        // Wake threads that have reached their wakeup time
        woke_threads = wake_threads_by_time(p, current_time);
        
        if (woke_threads > 0) {
            TRACE_THREAD("EXIT: Woke up %d threads from sleep queue", woke_threads);
        }
    }
    
    // STEP 2: Check the ready queue for the next thread to run
    next_thread = p->ready_queue;
    
    // Make sure the next thread is valid
    while (next_thread && (next_thread->magic != CTXT_MAGIC || 
                          (next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("EXIT: Skipping invalid thread %d in ready queue", next_thread->tid);
        remove_from_ready_queue(next_thread);
        next_thread = p->ready_queue;
    }
    
    if (next_thread) {
        TRACE_THREAD("EXIT: Found next thread %d in ready queue", next_thread->tid);
        remove_from_ready_queue(next_thread);
        return next_thread;
    }
    
    // STEP 3: Always try to find thread0 first (unless we only have 1 thread left)
    if (p->num_threads > 1) {
        TRACE_THREAD("EXIT: Looking for thread0 (num_threads=%d)", p->num_threads);
        struct thread *t;
        for (t = p->threads; t; t = t->next) {
            if (t->tid == 0 && 
                t->magic == CTXT_MAGIC && 
                !(t->state & THREAD_STATE_EXITED) &&
                t->wait_type == WAIT_NONE) {
                next_thread = t;
                TRACE_THREAD("EXIT: Found thread0 at %p, state=%d, wait_type=%d", 
                            next_thread, next_thread->state, next_thread->wait_type);
                break;
            }
        }
    }
    
    // STEP 4: Only create idle thread if no thread0 and there are joinable threads
    if (!next_thread) {
        struct thread *t;
        int has_joinable_threads = 0;
        for (t = p->threads; t; t = t->next) {
            if (t->magic == CTXT_MAGIC && t->joiner && t->joiner->magic == CTXT_MAGIC) {
                has_joinable_threads = 1;
                break;
            }
        }
        
        if (has_joinable_threads) {
            TRACE_THREAD("FIND NEXT THREAD: Creating idle thread for joinable threads");
            next_thread = get_idle_thread(p);
        } else {
            TRACE_THREAD("FIND NEXT THREAD: No threads available");
        }
    }
    return next_thread;
}

/**
 * Clean up thread resources during thread exit
 */
void cleanup_thread_resources(struct proc *p, struct thread *t, int tid) {
    if (!p || !t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("EXIT: Cleaning up resources: Invalid thread %d", tid);
        return;
    }
    TRACE_THREAD("EXIT: Cleaning up resources for thread %d", tid);

    /* Clean up sleep state */
    cleanup_thread_sleep_state(t);

    /* Clean up signal stack */
    cleanup_signal_stack(p, (long)t);

    /* Clean up thread signal resources */
    cleanup_thread_signals(t);

    /* Clean up thread cleanup handlers */
    cleanup_thread_handlers(t);

    /* Clean up cancellation state */
    cleanup_thread_cancellation(t);

    /* Clean up thread-specific data */
    cleanup_thread_tsd(t);

    /* Cancel all remaining timeouts for this thread */
    cancel_thread_timeouts(p, t);

    // Clear thread signal state
    t->t_sigpending = 0;
    THREAD_SIGMASK_SET(t, 0);
    t->t_sig_in_progress = 0;
    
    int should_free = (t->detached || t->joined) && tid != 0 && !(t->joiner != NULL && t->joiner->magic == CTXT_MAGIC);
    TRACE_THREAD("EXIT: Thread %d detached=%d, joined=%d, has_joiner=%d, should_free=%d", 
                tid, t->detached, t->joined, (t->joiner != NULL), should_free);
    
    // Remove from thread list if detached or joined
    if (should_free) {
        struct thread **tp;
        for (tp = &p->threads; *tp; tp = &(*tp)->next) {
            if (*tp == t) {
                *tp = t->next;
                break;
            }
        }
    }
    TRACE_THREAD("EXIT: Removed thread %d from thread list", tid);
    
    // Clear current_thread pointer to prevent use after free
    if (p->current_thread == t) {
        p->current_thread = NULL;
    }
    TRACE_THREAD("EXIT: Cleared current_thread pointer for thread %d", tid);
    
    // Free resources if detached or joined and no joiner
    if (should_free) {
        if (t->stack && tid != 0) {
            TRACE_THREAD("EXIT: Freeing stack for thread %d", tid);
            kfree(t->stack);
            t->stack = NULL;
        }
        
        // Clear magic BEFORE freeing to prevent use after free
        t->magic = 0;
        
        kfree(t);
        TRACE_THREAD("EXIT: KFREE thread %d", tid);
    } else {
        TRACE_THREAD("EXIT: Thread %d not detached or joined or has joiner, keeping resources", tid);
    }
}

/**
 * Thread exit function
 */
void proc_thread_exit(void *retval, void *arg) {

    struct proc *p = curproc;
    
    if (!p) {
        TRACE_THREAD("EXIT ERROR: No current process");
        return;
    }

    struct thread *current = NULL;

    if(!arg) {
        current = p->current_thread;
        TRACE_THREAD("EXIT: Thread %d is exiting (EXIT THREAD)", current->tid);
    } else {
        current = (struct thread *)arg;
        TRACE_THREAD("EXIT: Thread %d is exiting (CANCEL THREAD)", current->tid);
    }

    if (!current) {
        TRACE_THREAD("EXIT ERROR: No current thread");
        return;
    }

    static int thread_exit_in_progress = 0;
    static int exit_owner_tid = -1;

    int tid = current->tid;
    TRACE_THREAD("EXIT: Thread %d is exiting with retval=%p", tid, retval);
    register unsigned short sr = splhigh();

    // Protection against reentrance
    if (thread_exit_in_progress && exit_owner_tid != current->tid) {
        spl(sr);
        TRACE_THREAD("EXIT: Thread exit already in progress by thread %d, waiting", exit_owner_tid);
        proc_thread_exit(retval, NULL); // Pass retval to recursive call
        return;
    }

    // Check if the thread is already exited or freed
    if (current->magic != CTXT_MAGIC || (current->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("WARNING: proc_thread_exit: Thread %d already exited or freed (magic=%lx, state=%d)",
                    tid, current->magic, current->state);
        spl(sr);
        return;
    }
    
    if (current->tid == 0) {
        if (p->num_threads > 1) {
            TRACE_THREAD("EXIT: Preventing thread0 exit while other threads exist (num_threads=%d)", 
                        p->num_threads);
            spl(sr);
            return;
        }
        TRACE_THREAD("EXIT: Allowing thread0 to exit - no other threads remain");
    }

    thread_exit_in_progress = 1;
    exit_owner_tid = tid;
    TRACE_THREAD("EXIT: Thread %d is beginning exit process", tid);

    // Cancel timeouts associated with this thread
    TRACE_THREAD("EXIT: Cancelling timeouts for thread %d", tid);
    cancel_thread_timeouts(p, current);

    // Additional safety: clear sleep state immediately
    if (current->wait_type & WAIT_SLEEP) {
        current->wait_type &= ~WAIT_SLEEP;
        current->wakeup_time = 0;
        remove_from_sleep_queue(p, current);
    }

    // Check for pending signals before exiting
    if (current->t_sigpending) {
        int sig = check_thread_signals(current);
        if (sig && current->sig_handlers[sig].handler) {
            TRACE_THREAD("EXIT: Thread %d has pending signal %d, handling before exit", 
                        current->tid, sig);
            handle_thread_signal(current, sig);
        }
    }

    TRACE_THREAD("EXIT: Running cleanup handlers for thread %d", current->tid);
    run_cleanup_handlers(current);  // Execute all cleanup handlers automatically
    TRACE_THREAD("EXIT: Running tsd destructors for thread %d", current->tid);
    run_tsd_destructors(current);  // User-space destructor handler

    // Store the return value in the thread structure
    current->retval = retval;

    // Handle thread joining
    TRACE_THREAD("EXIT: Handling thread joining for thread %d", current->tid);
    handle_thread_joining(current, retval);

    // Remove from all queues
    TRACE_THREAD("EXIT: Removing thread %d from wait queues and ready queue", current->tid);
    remove_thread_from_wait_queues(current);
    remove_from_ready_queue(current);
    
    // Mark thread as exited
    TRACE_THREAD("EXIT: Marking thread %d as exited", current->tid);
    atomic_thread_state_change(current, THREAD_STATE_EXITED);

    // Special handling for idle thread exit
    if (current->is_idle) {
        TRACE_THREAD("EXIT: Idle thread %d is exiting", tid);
        // Check if there are still threads that need joining
        struct thread *t;
        int has_joinable_threads = 0;
        for (t = p->threads; t; t = t->next) {
            if (t->magic == CTXT_MAGIC && t->joiner && t->joiner->magic == CTXT_MAGIC) {
                has_joinable_threads = 1;
                break;
            }
        }
        
        if (!has_joinable_threads) {
            TRACE_THREAD("EXIT: Idle thread no longer needed, exiting");
        }
    } else if(tid > 0) {
        p->num_threads--;
    }

    TRACE_THREAD("EXIT: Thread %d exited, num_threads=%d", tid, p->num_threads);
    
    // Handle timers
    if (p->num_threads == 1) {
        TRACE_THREAD("EXIT: Only one thread remaining, stopping all timers");
        if (thread_switch_timeout) {
            TRACE_THREAD("EXIT: Cancelling thread switch timeout");
            canceltimeout(thread_switch_timeout);
            thread_switch_timeout = NULL;
        }
        if (p->p_thread_timer.enabled) {
            TRACE_THREAD("EXIT: Stopping thread timer for process %d", p->pid);
            thread_timer_stop(p);
        }
    }
    
    // Find next thread to run
    CONTEXT *target_ctx = NULL;
    struct thread *next_thread = find_next_thread_to_run(p);
    TRACE_THREAD("EXIT: Found next thread %d to run after exit", 
                next_thread ? next_thread->tid : -1);

    
    if (next_thread){
        // Check if the next thread is cancellable
        TRACE_THREAD("EXIT: Checking if next thread %d is cancellable", next_thread->tid);
        check_thread_cancellation(next_thread);
    }
    // If we found a valid next thread, prepare to switch to it
    if (next_thread && next_thread->magic == CTXT_MAGIC && !(next_thread->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("EXIT: Will switch to thread %d", next_thread->tid);
        atomic_thread_state_change(next_thread, THREAD_STATE_RUNNING);
        p->current_thread = next_thread;
        target_ctx = get_thread_context(next_thread);
        if (!target_ctx) {
            TRACE_THREAD("EXIT ERROR: Could not get context for thread %d", next_thread->tid);
            next_thread = NULL;
        } else {
            reset_thread_priority(next_thread);
            TRACE_THREAD("EXIT: Switching to thread %d context, PC=%lx", 
                        next_thread->tid, target_ctx->pc);
        }
    }

#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
    if (!target_ctx) {
        TRACE_THREAD("EXIT: Checking for orphaned threads");
        check_orphaned_threads(p);
        // Try to find an orphaned thread to run
        TRACE_THREAD("EXIT: Finding next thread to run after checking for orphans");
        next_thread = find_next_thread_to_run(p);

        if (next_thread && next_thread->magic == CTXT_MAGIC && 
            !(next_thread->state & THREAD_STATE_EXITED)) {
            TRACE_THREAD("EXIT: Found orphaned thread %d to run", next_thread->tid);
            atomic_thread_state_change(next_thread, THREAD_STATE_RUNNING);
            p->current_thread = next_thread;
            target_ctx = get_thread_context(next_thread);
        }
    }
#endif

    // If no valid thread found, use process context
    if (!target_ctx) {
        TRACE_THREAD("EXIT: No next thread, returning to process context");
        p->current_thread = get_main_thread(p);
        target_ctx = get_thread_context(p->current_thread);
    }
    
    // Clean up thread resources
    TRACE_THREAD("EXIT: Cleaning up resources for exiting thread %d", tid);
    cleanup_thread_resources(p, current, tid);
    
    TRACE_THREAD("Thread %d exited", tid);
    
    thread_exit_in_progress = 0;
    exit_owner_tid = -1;
    
    // Switch to target context
    TRACE_THREAD("EXIT: Switching to target context, sr = %x, ssp = %lx, usp = %lx, pc = %lx", target_ctx->sr, target_ctx->ssp, target_ctx->usp, target_ctx->pc);
    
    // CRITICAL: Do NOT save context when exiting!
    // The exiting thread should never resume - just switch directly
    spl(sr);

    if((target_ctx->sr & 0x2000) == 0) leave_kernel();
    change_context(target_ctx);
    
    // Should NEVER reach here - if we do, it's a critical error
    TRACE_THREAD("CRITICAL ERROR: Returned from change_context after thread exit!");
    
    // Try to recover by scheduling another thread

    proc_thread_schedule();
}

static inline short should_schedule_thread(struct thread *current, struct thread *next) {
    if (!next) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Invalid thread");
        return 0;
    }
    
    // If no current thread, always schedule next thread
    if (!current) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): No current thread, scheduling next thread %d", 
                    next->tid);
        return 1;
    }

    /* Special case: idle thread is always preemptible by normal threads */
    if (current->is_idle && !next->is_idle) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Idle thread preempted by normal thread %d", next->tid);
        return 1;
    }
    
    /* Special case: thread0 is always preemptible by other non-idle threads */
    if (current->tid == 0 && next->tid != 0 && !next->is_idle) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): thread0 is current, allowing switch to thread %d", next->tid);
        return 1;
    }

    /* If current thread is not running, always schedule next thread */
    if ((current->state != THREAD_STATE_RUNNING) || (current->state & THREAD_STATE_BLOCKED)) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread %d is not running, scheduling next thread %d",
                    current->tid, next->tid);
        return 1;
    }

    /* Calculate elapsed time once */
    unsigned long elapsed = get_system_ticks() - current->last_scheduled;

    /* PRIORITY CHECK FIRST - Higher priority always preempts */
    if (next->priority > current->priority) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Higher priority thread %d (pri %d%s) preempting thread %d (pri %d)",
                    next->tid, next->priority, next->priority_boost ? " boosted" : "",
                    current->tid, current->priority);
        return 1;
    }

    /* RT threads always preempt SCHED_OTHER threads (regardless of numerical priority) */
    if ((next->policy == SCHED_FIFO || next->policy == SCHED_RR) &&
        current->policy == SCHED_OTHER) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): RT thread %d preempting SCHED_OTHER thread %d",
                    next->tid, current->tid);
        return 1;
    }

    /* Check minimum timeslice for equal/lower priority */
    if (next->priority <= current->priority && elapsed < current->proc->thread_min_timeslice) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread %d hasn't used minimum timeslice (%lu < %d)",
                    current->tid, elapsed, current->proc->thread_min_timeslice);
        return 0;
    }

    /* Equal priority handling */
    if (next->priority == current->priority) {
        /* SCHED_FIFO threads continue running until preempted by higher priority */
        if (current->policy == SCHED_FIFO) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): SCHED_FIFO thread %d continues (equal priority)",
                        current->tid);
            return 0;
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

    /* Lower priority threads don't preempt higher priority ones */
    return 0;
}

void thread_switch(struct thread *from, struct thread *to) {
    struct thread_switch_context ctx = {0};
    
    // Fast validation first
    if (!to) {
        TRACE_THREAD("SWITCH: Invalid destination thread pointer");
        return;
    }
    
    // Special case: if from is NULL, just switch to the destination thread
    if (!from) {
        CONTEXT *to_ctx = get_thread_context(to);
        to->last_scheduled = get_system_ticks();
        
        TRACE_THREAD("SWITCH: Switching to thread %d (no source thread)", to->tid);

        reset_thread_priority(to);

        if((to_ctx->sr & 0x2000) == 0) leave_kernel();
        change_context(to_ctx);
        TRACE_THREAD("SWITCH ERROR: Should not reach here");
        return;
    }
    
    if (from == to) {
        TRACE_THREAD("SWITCH: Source and destination threads are the same");
        return;
    }
    
    // Initialize context structure
    ctx.from = from;
    ctx.to = to;
    ctx.process = from->proc;
    ctx.switch_time = get_system_ticks();
    
    // PREPARATION PHASE - Outside critical section
    prepare_thread_switch(&ctx);
    
    // Execute the switch if preparation was successful
    if (ctx.to && ctx.to_ctx) {
        register unsigned short sr = splhigh();
        execute_thread_switch(&ctx);
        spl(sr);
    }
}

/* Add this new function to prepare thread switch outside critical section */
static void prepare_thread_switch(struct thread_switch_context *ctx) {
    TRACE_THREAD("SWITCH: Preparing switch from %d to %d", 
                ctx->from->tid, ctx->to->tid);
    
    // Check magic numbers and states in one go
    if (ctx->from->magic != CTXT_MAGIC || ctx->to->magic != CTXT_MAGIC ||
        (ctx->from->state & THREAD_STATE_EXITED) || (ctx->to->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("SWITCH: Invalid thread magic or exited state: from=%d, to=%d", 
                    ctx->from->tid, ctx->to->tid);
        ctx->to = NULL;
        return;
    }
    
    // Special handling for thread0
    if (ctx->from->tid == 0 && (ctx->from->state & THREAD_STATE_EXITED) && 
        ctx->from->proc->num_threads > 1) {
        TRACE_THREAD("SWITCH: Preventing thread0 exit while other threads running");
        atomic_thread_state_change(ctx->from, THREAD_STATE_READY);
        ctx->to = NULL;
        return;
    }
    
    // Verify stack integrity
    if (ctx->from->stack_magic != STACK_MAGIC || ctx->to->stack_magic != STACK_MAGIC) {
        TRACE_THREAD("SWITCH ERROR: Stack corruption detected!");
        ctx->to = NULL;
        return;
    }
    
    // Get destination context once
    ctx->to_ctx = get_thread_context(ctx->to);
    if (!ctx->to_ctx) {
        TRACE_THREAD("SWITCH: Failed to get context for thread %d", ctx->to->tid);
        ctx->to = NULL;
        return;
    }
}

/* Add this new function to execute thread switch in minimal critical section */
static void execute_thread_switch(struct thread_switch_context *ctx) {
    unsigned long now;
    // Check if another switch is in progress
    if (thread_switch_in_progress) {
        TRACE_THREAD("SWITCH: Another switch in progress, aborting");
        return;
    }
    
    // Set switch in progress and setup deadlock detection
    thread_switch_in_progress = 1;
    
    if (thread_switch_timeout) {
        canceltimeout(thread_switch_timeout);
    }
    thread_switch_timeout = addtimeout(ctx->from->proc, 
                                     ((ctx->from->proc->thread_default_timeslice * MS_PER_TICK) / 20), 
                                     thread_switch_timeout_handler);
    
    TRACE_THREAD("SWITCH: Switching threads: %d -> %d", ctx->from->tid, ctx->to->tid);

    // Update CPU time for outgoing thread
    now = get_system_ticks();

    if (ctx->from->last_scheduled == 0) {
        /*
         * First time this thread is switched out - it was never properly scheduled.
         * This happens for initial threads. Set last_scheduled to now to prevent
         * accounting the entire uptime, but don't add to CPU time.
         */
        ctx->from->last_scheduled = now;
        TRACE_THREAD("SWITCH: Initializing last_scheduled for thread %d", ctx->from->tid);
    }

    // Set new schedule time for incoming thread
    ctx->to->last_scheduled = now;
    TRACE_THREAD("SWITCH: Thread %d scheduled at %lu (was %lu)", ctx->to->tid, now, ctx->to->last_scheduled);
    if (
        (ctx->from->priority_boost 
        && get_system_ticks() - ctx->from->last_scheduled > ctx->from->proc->thread_min_timeslice) 
        && (ctx->from->wait_type == WAIT_NONE)
    ) 
    {
        reset_thread_priority(ctx->from);
    }

    /* Check for pending signals in the thread we're switching to */
    if (ctx->to->proc->p_sigacts && ctx->to->proc->p_sigacts->thread_signals) {
        /* Skip thread0 - it handles process signals */
        /* Also skip threads that haven't run yet (last_scheduled == 0) */
        if (ctx->to->tid > 0 && ctx->to->last_scheduled > 0) {
            dispatch_thread_signals(ctx->to);
        }
    }
    
    // Handle context switch based on thread state
    if ((ctx->from->wait_type & WAIT_SLEEP) || (ctx->from->wait_type & WAIT_JOIN) || (ctx->from->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("SWITCH: Thread %d is sleeping, joining or waiting on semaphore, skipping switch", ctx->from->tid);
        // Sleeping thread path - direct context switch
        atomic_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
        ctx->from->proc->current_thread = ctx->to;
        reset_thread_priority(ctx->to);
        reset_thread_switch_state();

        TRACE_THREAD("SWITCH: Switched to context for thread %d, sr = %x, ssp = %lx, usp = %lx, pc = %lx", ctx->to->tid, ctx->to_ctx->sr, ctx->to_ctx->ssp, ctx->to_ctx->usp, ctx->to_ctx->pc);

        if((ctx->to_ctx->sr & 0x2000) == 0) leave_kernel();
        change_context(ctx->to_ctx);
        
        TRACE_THREAD("SWITCH ERROR: Returned from change_context!");
    } else {
        if (ctx->from->wait_type == WAIT_NONE) {
            atomic_thread_state_change(ctx->from, THREAD_STATE_READY);
        }
        CONTEXT *from_ctx = get_thread_context(ctx->from);
        
        // Validate stack alignment before save (M68K requires even addresses)
        if ((from_ctx->ssp & 1) || (from_ctx->usp & 1)) {
            TRACE_THREAD("SWITCH ERROR: Invalid stack alignment - SSP=%lx, USP=%lx", 
                        from_ctx->ssp, from_ctx->usp);
            ctx->to = NULL;
            return;
        }
        TRACE_THREAD("PRE-SAVE: Thread %d context - SSP=%lx, USP=%lx, PC=%lx", 
                    ctx->from->tid, from_ctx->ssp, from_ctx->usp, from_ctx->pc);
        
        if (save_context(from_ctx) == 0) {
            from_ctx->regs[0] = 1;

            TRACE_THREAD("SWITCH: FIRST TIME - Saved context successfully for thread %d, ssp = %lx, usp = %lx, pc = %lx, sr = %x, to = %d", ctx->from->tid, from_ctx->ssp, from_ctx->usp, from_ctx->pc, from_ctx->sr, ctx->to->tid);
            
            // // Only change state if not blocked on mutex/semaphore
            // if(ctx->to->state & THREAD_STATE_EXITED) {
            //     TRACE_THREAD("SWITCH: Thread %d is exiting, skipping switch", ctx->to->tid);
            //     atomic_thread_state_change(ctx->from, THREAD_STATE_RUNNING);
            //     ctx->to = NULL;
            //     return;
            // }

            atomic_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
            ctx->from->proc->current_thread = ctx->to;

            reset_thread_priority(ctx->to);

            reset_thread_switch_state();

            TRACE_THREAD("SWITCH: Switched to context for thread %d, ssp = %lx, usp = %lx, pc = %lx, sr = %x", ctx->to->tid, ctx->to_ctx->ssp, ctx->to_ctx->usp, ctx->to_ctx->pc, ctx->to_ctx->sr);
            
            if((ctx->to_ctx->sr & 0x2000) == 0) {
                TRACE_THREAD("SWITCH: Leaving kernel mode for thread %d", ctx->to->tid);
                leave_kernel();
            }
            change_context(ctx->to_ctx);
            
            TRACE_THREAD("SWITCH ERROR: Returned from change_context!");
        } else {

            TRACE_THREAD("SWITCH: SECOND TIME - Thread %d resumed from context switch at pc=%lx", ctx->from->tid, from_ctx->pc);
            // If we failed to save context, we can't switch
            atomic_thread_state_change(ctx->from, THREAD_STATE_RUNNING);
            ctx->to = NULL;
            
        }
    }

    TRACE_THREAD("SWITCH: Return path after being switched back");
    // Return path after being switched back
    reset_thread_switch_state();
}

/* Add these functions to optimize proc_thread_schedule */
static int prepare_scheduling_decision(struct proc *p, struct scheduling_decision *decision) {
    decision->current_thread = p->current_thread;
    decision->decision_time = get_system_ticks();
    
    if (!decision->current_thread || 
        decision->current_thread->last_scheduled + time_slice < decision->decision_time) {
        // Check and wake any sleeping threads
        check_and_wake_sleeping_threads(p);
    }

    TRACE_THREAD("SCHED: Current thread state=%d wait_type=%d", 
                 decision->current_thread->state, decision->current_thread->wait_type);
    if (decision->current_thread->wait_type) {
        TRACE_THREAD("SCHED: Thread %d is waiting on: %s", 
                     decision->current_thread->tid,
                     (decision->current_thread->wait_type & WAIT_MUTEX) ? "MUTEX" :
                     (decision->current_thread->wait_type & WAIT_CONDVAR) ? "CONDVAR" :
                     (decision->current_thread->wait_type & WAIT_JOIN) ? "JOIN" : "UNKNOWN");
    }

    // Get highest priority thread from ready queue
    decision->next_thread = get_highest_priority_thread(p);
    
    // Validate next thread
    if (decision->next_thread && (decision->next_thread->magic != CTXT_MAGIC || 
                                 (decision->next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("SCHED: Next thread %d is invalid or exited, removing from ready queue", 
                    decision->next_thread->tid);
        remove_from_ready_queue(decision->next_thread);
        decision->next_thread = NULL;
    }
#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
    if (!decision->next_thread) {
        TRACE_THREAD("SCHED: No valid next thread found in ready queue, checking for orphans");
        // If no next thread found, check for orphaned threads
        // This is necessary to ensure we don't miss runnable threads that are not in the ready queue
        // Orphaned threads are those that are not in the ready queue but are runnable
        check_orphaned_threads(p);
        TRACE_THREAD("SCHED: After checking orphans, looking for next thread again");
        decision->next_thread = get_highest_priority_thread(p);
    }
#endif
    // Handle case where no next thread is found
    if (!decision->next_thread) {
        // Try to find thread0 first
        struct thread *thread0 = get_main_thread(p);
        
        if (thread0 && !(thread0->state & THREAD_STATE_EXITED) && (thread0->wait_type == WAIT_NONE)) {
            decision->next_thread = thread0;
            TRACE_THREAD("SCHED: Falling back to thread0");
        } else if (decision->current_thread && !(decision->current_thread->state & THREAD_STATE_BLOCKED) && !decision->current_thread->is_idle) {
            TRACE_THREAD("SCHED: Continuing with current thread %d",  decision->current_thread->tid);
            decision->current_thread->last_scheduled = decision->decision_time;
            return 0; // No switch needed
        } else {
            TRACE_THREAD("SCHED: No threads available");
            return 0;            
        }
    }
    
    // Check if we should schedule next thread
    decision->should_switch = should_schedule_thread(decision->current_thread, decision->next_thread);
    TRACE_THREAD("SCHED: Should switch: %d", decision->should_switch);
    return decision->should_switch;
}

static void execute_scheduling_decision(struct proc *p, struct scheduling_decision *decision) {
    if (!decision->should_switch || !decision->next_thread) {
        TRACE_THREAD("SCHED: No switch needed");
        return;
    }
    
    TRACE_THREAD("SCHED: Executing switch from %d to %d", 
                decision->current_thread ? decision->current_thread->tid : -1, 
                decision->next_thread->tid);

    // register unsigned short sr = splhigh();

    // Remove next from ready queue if it's there
    if (is_in_ready_queue(decision->next_thread)) {
        remove_from_ready_queue(decision->next_thread);
    }
    
    // Update thread states and prepare for switch
    if (decision->current_thread) {
        // Update timeslice accounting for current thread
        update_thread_timeslice(decision->current_thread);
        
        // Handle current thread based on its state
        if (decision->current_thread->state == THREAD_STATE_RUNNING) {
            if (decision->current_thread->wait_type != WAIT_NONE) {

                atomic_thread_state_change(decision->current_thread, THREAD_STATE_BLOCKED);
                
                // Priority inheritance for mutexes
                if ((decision->current_thread->wait_type & WAIT_MUTEX) && 
                    decision->current_thread->mutex_wait_obj) {
                    struct mutex *m = (struct mutex*)decision->current_thread->mutex_wait_obj;
                    if (m->owner && m->owner->priority < decision->current_thread->priority) {
                        boost_thread_priority(m->owner, 
                                            decision->current_thread->priority - m->owner->priority);
                        
                        // Reinsert owner in ready queue if needed
                        if (m->owner->state == THREAD_STATE_READY) {
                            remove_from_ready_queue(m->owner);
                            add_to_ready_queue(m->owner);
                        }
                    }
                }
            } else {
                // Thread is runnable but being preempted
                atomic_thread_state_change(decision->current_thread, THREAD_STATE_READY);
                add_to_ready_queue(decision->current_thread);
            }
        }
    }
    
    // Update next thread state
    atomic_thread_state_change(decision->next_thread, THREAD_STATE_RUNNING);
    p->current_thread = decision->next_thread;

    // Reschedule preemption timer if needed
    if (p->p_thread_timer.in_handler) {
        if (!p->p_thread_timer.enabled) {
            TRACE_THREAD("SCHEDULER -> PREEMPT: Timer disabled, not rescheduling");
        }
        TRACE_THREAD("SCHEDULER -> PREEMPT: Already in handler, rescheduling");
        reschedule_preemption_timer(p, (long)p->current_thread);
    }

    // Use the original thread_switch function for now
    // This ensures compatibility until optimized_thread_switch is fully tested
    // spl(sr);
    thread_switch(decision->current_thread, decision->next_thread);
}


/**
 * Helper function to reschedule the preemption timer
 */
void reschedule_preemption_timer(PROC *p, long arg) {
    if (!p){
        TRACE_THREAD("SCHED_TIMER: Invalid process reference");
        return;
    }

    // Cancel existing timeout first
    if (p->p_thread_timer.timeout) {
        canceltimeout(p->p_thread_timer.timeout);
        p->p_thread_timer.timeout = NULL;
    }

    struct thread *t = (struct thread *)arg;
    // TRACE_THREAD("SCHED_TIMER: Rescheduling preemption timer for process %d, arg tid %d", p->pid, t->tid);
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, thread_preempt_handler);
    p->p_thread_timer.in_handler = 0;
    if (p->p_thread_timer.timeout) {
        p->p_thread_timer.timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SCHED_TIMER: Failed to reschedule preemption timer for process %d", p->pid);
    }
}

/*
 * Reset the thread switch state
 * Called when a thread switch completes or times out
 */
static void reset_thread_switch_state(void) {

    register unsigned short sr = splhigh();
    
    thread_switch_in_progress = 0;
    
    if (thread_switch_timeout) {
        canceltimeout(thread_switch_timeout);
        thread_switch_timeout = NULL;
    }
    
    spl(sr);
}

/*
 * Thread switch timeout handler
 * Called when a thread switch takes too long
 */
static void thread_switch_timeout_handler(PROC *p, long arg) {

    static int recovery_attempts = 0;
    
    TRACE_THREAD("TIMEOUT: Thread switch timed out after %dms", ((p->thread_default_timeslice * 5) / 20));
    
    if (++recovery_attempts > MAX_SWITCH_RETRIES) {
        TRACE_THREAD("CRITICAL: Max recovery attempts reached, system may be unstable");
        
        // More aggressive recovery - try to restore a known good state
        if (p && p->current_thread && p->current_thread->magic == CTXT_MAGIC) {
            CONTEXT *ctx = get_thread_context(p->current_thread);
            TRACE_THREAD("TIMEOUT: Attempting to restore current thread %d",  p->current_thread->tid);
            if((ctx->sr & 0x2000) == 0) leave_kernel();
            change_context(ctx);

        }
        
        recovery_attempts = 0;
    }
    
    /* Force reset of thread switch state */
    reset_thread_switch_state();
    
    /* Schedule next thread to try to recover */
    proc_thread_schedule();
}

/*
 * Start timing a specific process/thread
 */
void thread_timer_start(struct proc *p, int thread_id) {
    register unsigned short sr, retry_count = 0;
    
    TRACE_THREAD("TIMER: thread_timer_start called for process %d", p->pid);
    if (!p)
        return;

    /* Try to acquire the timer operation lock with a timeout */
    while (1) {
        sr = splhigh();
        if (!timer_operation_locked) {
            TRACE_THREAD("TIMER: Acquired timer operation lock");
            timer_operation_locked = 1;
            spl(sr);
            break;
        }
        spl(sr);
        
        /* If we've tried too many times, give up */
        if (++retry_count > 10) {
            TRACE_THREAD("TIMER WARNING: Failed to acquire timer operation lock after 10 retries");
            return;
        }
    }

    /* CRITICAL SECTION - We now have the timer operation lock */
    TRACE_THREAD("TIMER: Starting thread timer for process %d", p->pid);
    
    sr = splhigh();
    
    /* If timer is already enabled, don't add another timeout */
    if (p->p_thread_timer.enabled && p->p_thread_timer.timeout) {
        TRACE_THREAD("TIMER: Timer already enabled, not adding another timeout");
        spl(sr);
        goto cleanup;
    }
    
    /* Create the timeout before modifying any state */
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, thread_preempt_handler);
    if (!(p->p_thread_timer.timeout)) {
        TRACE_THREAD("TIMER ERROR: Failed to create timeout");
        spl(sr);
        goto cleanup;
    }
    p->p_thread_timer.timeout->arg = (long)p->current_thread;

    /* Now that we have a valid timeout, update the timer state */
    p->p_thread_timer.thread_id = p->current_thread->tid;

    /* Set enabled flag last to ensure everything is set up */
    p->p_thread_timer.enabled = 1;
    p->p_thread_timer.in_handler = 0;
    
    TRACE_THREAD("TIMER: Thread timer started for process %d with interval %dms", p->pid, p->thread_preempt_interval);
    spl(sr);
    
cleanup:
    /* Always release the timer operation lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    
    TRACE_THREAD("TIMER: Timer timer operation lock released");
}

/*
 * Stop the thread timer
 */
void thread_timer_stop(PROC *p)
{
    register unsigned short sr;
    TIMEOUT *timeout_to_cancel = NULL;
    int retry_count = 0;

    if (!p) {
        return;
    }

    /* Try to acquire the timer operation lock with a timeout */
    while (1) {
        sr = splhigh();
        if (!timer_operation_locked) {
            timer_operation_locked = 1;
            spl(sr);
            break;
        }
        spl(sr);
        
        /* If we've tried too many times, give up */
        if (++retry_count > 10) {
            TRACE_THREAD("TIMER WARNING: Failed to acquire timer operation lock after 10 retries");
            return;
        }

    }

    /* CRITICAL SECTION - We now have the operation lock */
    TRACE_THREAD("TIMER: Stopping thread timer for process %d", p->pid);
    
    sr = splhigh();
    
    /* Check if timer is already disabled */
    if (p->num_threads <= 1) {
        TRACE_THREAD("TIMER: Disabling timer for process %d (num_threads=%d)", p->pid, p->num_threads);
        p->p_thread_timer.enabled = 0;
    }
    
    /* Save the timeout pointer locally before clearing it */
    timeout_to_cancel = p->p_thread_timer.timeout;
    p->p_thread_timer.timeout = NULL;
    TRACE_THREAD("TIMER: Cleared timeout pointer");
    
    spl(sr);
    
    /* Cancel the timeout outside the critical section */
    if (timeout_to_cancel) {
        canceltimeout(timeout_to_cancel);
        TRACE_THREAD("TIMER: Cancelled timeout");
    }
    
    /* Always release the timer operation lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    
    TRACE_THREAD("TIMER: Timer operation lock released");
}

#if THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE
static inline short fix_orphaned_thread(struct thread *t, struct proc *p) {
    if (!t || !p ) return 0;

    if(!p->threads|| t->magic != CTXT_MAGIC || t->tid == 0) return 0;

    register unsigned short sr = splhigh();
    short fixed = 0;

    // Cas 1: Thread en cours d'exécution mais pas dans la ready queue
    if (t->state == THREAD_STATE_RUNNING && 
        !is_in_ready_queue(t) &&
        t->wait_type == WAIT_NONE) {
        TRACE_THREAD("ORPHAN: Thread %d running but not in ready queue", t->tid);
        atomic_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);
        fixed = 1;
    }

    // Cas 2: Thread prêt mais pas dans la ready queue
    else if (t->state == THREAD_STATE_READY && 
             !is_in_ready_queue(t) &&
             t->wait_type == WAIT_NONE) {
        TRACE_THREAD("ORPHAN: Thread %d ready but not in ready queue", t->tid);
        add_to_ready_queue(t);
        fixed = 1;
    }

    // Cas 3: Thread bloqué avec état incohérent
    else if (t->state == THREAD_STATE_BLOCKED) {
        short orphaned = 0;
        
        // Vérifier chaque type d'attente
        if (t->wait_type & WAIT_MUTEX) {
            struct mutex *m = t->mutex_wait_obj;
            if (!m || !m->wait_queue || !is_in_wait_queue(m->wait_queue, t)) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid mutex", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type & WAIT_SEMAPHORE) {
            struct semaphore *sem = t->sem_wait_obj;
            if (!sem || !sem->wait_queue || !is_in_wait_queue(sem->wait_queue, t)) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid semaphore", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type & WAIT_CONDVAR) {
            struct condvar *cond = t->cond_wait_obj;
            if (!cond || !cond->wait_queue || !is_in_wait_queue(cond->wait_queue, t)) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid condvar", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type & WAIT_SIGNAL) {
            if (!is_in_signal_wait_queue(p, t)) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid signal wait", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type & WAIT_JOIN) {
            struct thread *target = t->join_wait_obj;
            if (!target || target->magic != CTXT_MAGIC || target->joiner != t) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid join target", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type & WAIT_SLEEP) {
            if (!is_in_sleep_queue(p, t)) {
                TRACE_THREAD("ORPHAN: Thread %d blocked on invalid sleep", t->tid);
                orphaned = 1;
            }
        }
        else if (t->wait_type != WAIT_NONE) {
            TRACE_THREAD("ORPHAN: Thread %d blocked with unknown wait type %d", 
                        t->tid, t->wait_type);
            orphaned = 1;
        }
        
        if (orphaned) {
            remove_thread_from_specific_wait_queue(t, t->wait_type);
            t->wait_type = WAIT_NONE;
            atomic_thread_state_change(t, THREAD_STATE_READY);
            add_to_ready_queue(t);
            fixed = 1;
        }
    }

    // Cas 4: Thread dans une queue mais avec mauvais état
    if (!fixed) {
        if (is_in_ready_queue(t) && t->state != THREAD_STATE_READY) {
            TRACE_THREAD("ORPHAN: Thread %d in ready queue but state %d", 
                        t->tid, t->state);
            atomic_thread_state_change(t, THREAD_STATE_READY);
            fixed = 1;
        }
        else if (is_in_sleep_queue(p, t) && !(t->wait_type & WAIT_SLEEP)) {
            TRACE_THREAD("ORPHAN: Thread %d in sleep queue but not sleeping", t->tid);
            remove_from_sleep_queue(p, t);
            fixed = 1;
        }
    }

    spl(sr);
    return fixed;
}

static void check_orphaned_threads(struct proc *p) {
    if (!p) return;

    struct thread *t;
    int orphans_found = 0;

    TRACE_THREAD("ORPHAN CHECK: Scanning threads for process %d", p->pid);
    for (t = p->threads; t != NULL; t = t->next) {
        if (fix_orphaned_thread(t, p)) {
            TRACE_THREAD("ORPHAN CHECK: Fixed orphaned thread %d, state %d, wait type %d", t->tid, 
                        t->state, t->wait_type);
            orphans_found++;
        }
    }

    if (orphans_found) {
        TRACE_THREAD("ORPHAN CHECK: Found and fixed %d orphaned threads", 
                    orphans_found);
    }
}
#endif // THREAD_DEBUG_LEVEL >= THREAD_DEBUG_VERBOSE