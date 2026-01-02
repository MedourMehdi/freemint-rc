/******************************************************************************/
/* proc_threads.c - Kernel Thread Management Core (m68k Optimized)           */
/*                                                                            */
/* Implements core thread operations including:                               */
/*  - Thread creation and termination                                         */
/*  - Context initialization                                                   */
/*  - Idle thread management                                                  */
/*  - Process cleanup for multi-threaded processes                            */
/*  - Thread status and ID retrieval                                          */
/*                                                                            */
/* Optimization strategy:                                                     */
/*  - Single mint_bzero per allocation, set only non-zero fields             */
/*  - Minimize interrupt-disabled sections (splhigh/spl pairs)                */
/*  - Reduce redundant memory operations                                      */
/*  - Keep code paths straightforward for better branch prediction            */
/*                                                                            */
/* Author: Medour Mehdi                                                       */
/* Optimized for: Motorola 68000 architecture                                 */
/* Date: December 2025                                                        */
/******************************************************************************/

#include "proc_threads.h"
#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_sync.h"
#include "proc_threads_signal.h"
#include "proc_threads_tsd.h"
#include "proc_threads_cleanup.h"

#define PTHREAD_INHERIT_SCHED   0
#define PTHREAD_EXPLICIT_SCHED  1

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

/* Thread attribute type */
typedef struct {
    size_t stacksize;
    int detachstate;
    int policy;
    int priority;
    int inheritsched;    /* PTHREAD_INHERIT_SCHED or PTHREAD_EXPLICIT_SCHED */
} pthread_attr_t;

/* Forward declarations */
static void proc_thread_start(void);
static long create_thread(struct proc *p, void *(*func)(void*), void *arg, void *stack_ptr);
static void init_main_thread_context(struct proc *p);
static void init_thread_context(struct thread *t, void *(*func)(void*), void *arg);
static struct thread* create_idle_thread(struct proc *p);
static void *idle_thread_func(void *arg);

/******************************************************************************/
/* kernel_pthread_syscall - Issue pthread system call from kernel mode       */
/******************************************************************************/
static void kernel_pthread_syscall(unsigned long subsystem, unsigned long op, 
                                   unsigned long arg1, unsigned long arg2) {
    __asm__ volatile (
        "movl   %3, %%sp@-\n\t"         /* Push arg2 */
        "movl   %2, %%sp@-\n\t"         /* Push arg1 */
        "movl   %1, %%sp@-\n\t"         /* Push operation */
        "movl   %0, %%sp@-\n\t"         /* Push subsystem ID */
        "movw   #0x185, %%sp@-\n\t"     /* Push P_PTHREAD */
        "trap   #1\n\t"                 /* Make system call */
        "lea    %%sp@(18), %%sp"        /* Clean up stack */
        :
        : "g"(subsystem), "g"(op), "g"(arg1), "g"(arg2)
        : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory"
    );
}

/******************************************************************************/
/* proc_thread_start - Thread entry trampoline                                */
/* Executed when a new thread first runs. Calls user function and exits.     */
/******************************************************************************/
static void proc_thread_start(void) {
    struct thread *t;
    struct proc *p;
    void *(*func)(void*);
    void *arg;
    void *result = NULL;

    p = curproc;
    t = p ? p->current_thread : NULL;
    
    TRACE_THREAD("START: Thread trampoline started, thread=%p, tid=%d", 
                 t, t ? t->tid : -1);
    
    /* Validate thread and process */
    if (!t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("START: Invalid thread pointer %p or magic %lx", 
                     t, t ? t->magic : 0);
        return;
    }
    
    if (!p) {
        TRACE_THREAD("START: No process for thread %d", t->tid);
        return;
    }
    
    TRACE_THREAD("START: Current thread is %d", t->tid);
    
    /* Start preemption timer if needed (multi-threaded process) */
    if (p->num_threads > 1 && !p->p_thread_timer.enabled) {
        TRACE_THREAD("START: Starting thread timer for process %d (thread %d)", 
                     p->pid, t->tid);
        thread_timer_start(p, t->tid);
    }
   
    /* Extract function and argument (cached in registers on m68k) */
    func = t->func;
    arg = t->arg;

    TRACE_THREAD("START: Thread function=%p, arg=%p", func, arg);
    
    /* Call the thread function (user mode execution) */
    if (func) {
        TRACE_THREAD("START: Calling thread function");
        t->has_run = 1;
        
        result = func(arg);
        TRACE_THREAD("START: Thread function returned");
    }

    /* Exit thread with result */
    if (t && t->magic == CTXT_MAGIC) {
        TRACE_THREAD("START: Thread %d finished execution, result=%p", 
                     t->tid, result);
        kernel_pthread_syscall(P_THREAD_CTRL, THREAD_CTRL_EXIT, 
                              (unsigned long)result, 0);
    }
}

/******************************************************************************/
/* proc_thread_create - Thread creation syscall entry point                  */
/******************************************************************************/
long _cdecl proc_thread_create(void *(*func)(void*), void *arg, void *attr) {    
    TRACE_THREAD("CREATETHREAD: func=%p arg=%p attr=%p", func, arg, attr);

    /* Ensure main thread exists */
    init_main_thread_context(curproc);
    
    return create_thread(curproc, func, arg, attr);
}

/******************************************************************************/
/* create_thread - Internal thread creation implementation                    */
/* Creates a new user thread with optional attributes                         */
/******************************************************************************/
static long create_thread(struct proc *p, void *(*func)(void*), void *arg, 
                         void *attr_ptr) {
    register unsigned short sr;
    pthread_attr_t *attr = (pthread_attr_t *)attr_ptr;
    struct thread *t;
    size_t stack_size = STKSIZE;
    short is_detached = 0;
    short sched_policy = DEFAULT_SCHED_POLICY;
    short thread_priority = -1;
    short use_explicit_sched = 0;
    int calc_priority;

    /* Extract attributes if provided */
    if (attr) {
        if (attr->stacksize > 0) {
            stack_size = attr->stacksize;
        }
        is_detached = (attr->detachstate == PTHREAD_CREATE_DETACHED);
        use_explicit_sched = (attr->inheritsched == PTHREAD_EXPLICIT_SCHED);
        
        if (use_explicit_sched && attr->policy > 0) {
            sched_policy = attr->policy;
        }
        
        /* Validate and clamp priority */
        if (use_explicit_sched && attr->priority >= 0) {
            thread_priority = MIN(MAX(attr->priority, 1), MAX_THREAD_PRIORITY);
        }
        /* If inheriting, get from current thread */
        if (!use_explicit_sched && p->current_thread) {
            sched_policy = p->current_thread->policy;
            thread_priority = p->current_thread->priority;
            TRACE_THREAD("CREATETHREAD: Inheriting policy=%d, priority=%d from thread %d",
                        sched_policy, thread_priority, p->current_thread->tid);
        }
    }
    
    /* Validate process - do this BEFORE disabling interrupts */
    if (!p || p->magic != CTXT_MAGIC) {
        return EINVAL;
    }
    
    /* === CRITICAL SECTION START === */
    sr = splhigh();
    
    /* Allocate thread structure */
    t = kmalloc(sizeof(struct thread));
    if (!t) {
        spl(sr);
        return ENOMEM;
    }
    
    TRACE_THREAD("KMALLOC: Allocated thread structure at %p", t);
    TRACE_THREAD("Creating thread: pid=%d, func=%p, arg=%p, stack_size=%lu, "
                 "policy=%d, priority=%d", 
                 p->pid, func, arg, stack_size, sched_policy, thread_priority);
    
    /* Zero entire structure ONCE - this is efficient on m68k */
    mint_bzero(t, sizeof(*t));
    
    /* Allocate stack */
    t->stack = kmalloc(stack_size);
    if (!t->stack) {
        kfree(t);
        spl(sr);
        TRACE_THREAD("KFREE: Stack allocation failed");
        return ENOMEM;
    }
    
    TRACE_THREAD("KMALLOC: Allocated stack at %p for thread (size %zu)", 
                 t->stack, stack_size);
    
    /* === Set ONLY non-zero fields (optimization) === */
    
    /* Thread identity */
    t->tid = p->total_threads++;
    t->proc = p;
    t->magic = CTXT_MAGIC;
    
    /* Stack setup */
    t->stack_top = (char*)t->stack + stack_size;
    t->stack_size = stack_size;
    t->stack_magic = STACK_MAGIC;
    
    /* Priority calculation */
    if (thread_priority > 0) {
        t->priority = scale_thread_priority(thread_priority);
        t->original_priority = scale_thread_priority(thread_priority);
        TRACE_THREAD("Using %s priority %d for thread %d", 
                     use_explicit_sched ? "explicit" : "inherited",
                     thread_priority, t->tid);
    } else {
        calc_priority = (p->pri < 0) ? -p->pri : p->pri;
        t->priority = MAX(scale_thread_priority(calc_priority), 1);
        t->original_priority = t->priority;
    }
    
    /* Scheduling parameters */
    t->policy = sched_policy;
    t->timeslice = p->thread_default_timeslice;
    t->remaining_timeslice = p->thread_default_timeslice;
    
    /* Signal mask inheritance - inherit from current thread for POSIX compliance */
    if (p->current_thread && p->current_thread->magic == CTXT_MAGIC) {
        THREAD_SIGMASK_SET(t, THREAD_SIGMASK(p->current_thread));
    } else {
        THREAD_SIGMASK_SET(t, p->p_sigmask);
    }
    
    /* Join/detach state */
    t->detached = is_detached;
    
    /* Cancellation state (enabled by default for user threads) */
    t->cancel_state = PTHREAD_CANCEL_ENABLE;
    t->cancel_type = PTHREAD_CANCEL_DEFERRED;
    
    /* Link into process thread list (at head for O(1) insertion) */
    t->next = p->threads;
    p->threads = t;
    p->num_threads++;
    
    TRACE_THREAD("Thread %d stack: base=%p, top=%p, size=%zu", 
                 t->tid, t->stack, t->stack_top, stack_size);
    
    /* Initialize thread subsystems */
    init_thread_cleanup(t);
    init_thread_tsd(t);
    
    /* Initialize context (sets up registers and stack) */
    init_thread_context(t, func, arg);
    
    /* Make thread ready to run */
    proc_thread_state_change(t, THREAD_STATE_READY);

    if (!(p->p_flag & P_FLAG_THREADED)) {
        p->p_flag |= P_FLAG_THREADED;
        TRACE_THREAD("CREATETHREAD: Marked process %d as threaded", p->pid);
    }

    /* Start thread timer if not already running */
    if (!p->p_thread_timer.enabled) {
        thread_timer_start(p, t->tid);
    }
    
    /* Add to ready queue (scheduler will pick it up) */
    add_to_ready_queue(t);
    
    /* === CRITICAL SECTION END === */
    spl(sr);

    TRACE_THREAD_CREATE(t, t->func, t->arg);
    
    return t->tid;
}

/******************************************************************************/
/* init_thread_context - Initialize thread execution context                 */
/* Sets up stack and registers for first-time thread execution               */
/******************************************************************************/
static void init_thread_context(struct thread *t, void *(*func)(void*), 
                               void *arg) {
    unsigned long usp, ssp;
    unsigned long usp_size = ((unsigned long)t->stack_top - ((t->stack_size >> 1) - 256)); // Extra space for safety
    unsigned long ssp_size = ((unsigned long)t->stack_top - (t->stack_size - 256)); // Extra space for safety
    TRACE_THREAD("INIT CONTEXT: Initializing context for thread %d, stack_top - usp_size = %ld, ssp_size = %ld, ", t->tid, ((t->stack_size >> 1) - 256), (t->stack_size - 256));
    
    /* Clear contexts completely */
    mint_bzero(&t->ctxt[CURRENT], sizeof(t->ctxt[CURRENT]));
    mint_bzero(&t->ctxt[SYSCALL], sizeof(t->ctxt[SYSCALL]));
    
    /* Store function and argument in thread structure */
    t->func = func;
    t->arg = arg;
    
    /* Copy process context as template */
    memcpy(&t->ctxt[CURRENT], &t->proc->ctxt[CURRENT], sizeof(CONTEXT));
    memcpy(&t->ctxt[SYSCALL], &t->proc->ctxt[SYSCALL], sizeof(CONTEXT));

    /* Set up stack pointers (aligned to 4-byte boundary for m68k) */
    // usp = ((unsigned long)t->stack_top - 2048) & ~0x1UL;
    // ssp = ((unsigned long)t->stack_top - 4096) & ~0x1UL;
    usp = (usp_size) & ~0x1UL;
    ssp = (ssp_size) & ~0x1UL;    
    
    /* Initialize CURRENT context (what thread will run with) */
    t->ctxt[CURRENT].ssp = ssp;
    t->ctxt[CURRENT].usp = usp;
    t->ctxt[CURRENT].pc = (unsigned long)proc_thread_start;
    t->ctxt[CURRENT].sr = 0x0000;  /* User mode, interrupts enabled */
    
    /* Initialize SYSCALL context (saved during system calls) */
    t->ctxt[SYSCALL].ssp = ssp;
    t->ctxt[SYSCALL].usp = usp;
    t->ctxt[SYSCALL].pc = (unsigned long)proc_thread_start;
    t->ctxt[SYSCALL].sr = 0x0000;

    /* Initialize scheduling timestamp */
    t->last_scheduled = get_system_ticks();

    TRACE_THREAD("INIT CONTEXT: Thread %d initialized for USER MODE", t->tid);
    TRACE_THREAD(" CURRENT: SSP=%lx, USP=%lx, PC=%lx, SR=%04x", 
                t->ctxt[CURRENT].ssp, t->ctxt[CURRENT].usp, 
                t->ctxt[CURRENT].pc, t->ctxt[CURRENT].sr);
    TRACE_THREAD(" SYSCALL: SSP=%lx, USP=%lx, PC=%lx, SR=%04x", 
                t->ctxt[SYSCALL].ssp, t->ctxt[SYSCALL].usp, 
                t->ctxt[SYSCALL].pc, t->ctxt[SYSCALL].sr);
}

/******************************************************************************/
/* init_main_thread_context - Initialize thread0 (main thread) for process   */
/* Called once per process to create the initial execution context           */
/******************************************************************************/
static void init_main_thread_context(struct proc *p) {
    struct thread *t0;
    
    /* Idempotent - return if already initialized */
    if (p->current_thread) {
        return;
    }
    
    /* Allocate thread0 structure */
    t0 = kmalloc(sizeof(struct thread));
    if (!t0) {
        return;
    }
    
    TRACE_THREAD("KMALLOC: Allocated thread0 structure at %p for process %d", 
                 t0, p->pid);
    TRACE_THREAD("INIT CONTEXT: Initializing thread0 for process %d", p->pid);
    
    /* Zero entire structure ONCE */
    mint_bzero(t0, sizeof(*t0));
    
    /* === Set ONLY non-zero fields === */
    
    /* Thread0 identity (tid=0 is main thread) */
    t0->proc = p;
    t0->magic = CTXT_MAGIC;
    strncpy(t0->name, p->name, 15);
    t0->name[15] = '\0';
    
    /* Thread0 uses process stack (no separate allocation) */
    t0->stack = p->stack;
    t0->stack_top = (char*)p->stack + STKSIZE;
    t0->stack_size = STKSIZE;
    t0->stack_magic = STACK_MAGIC;
    
    /* Priority setup */
    t0->priority = MAX(scale_thread_priority(-p->pri), 1);
    t0->original_priority = t0->priority;
    t0->policy = DEFAULT_SCHED_POLICY;
    
    /* Scheduling timeslice */
    t0->timeslice = p->thread_default_timeslice;
    t0->remaining_timeslice = p->thread_default_timeslice;
    
    /* Initialize thread0 context from process context */
    memcpy(&t0->ctxt[CURRENT], &p->ctxt[CURRENT], sizeof(CONTEXT));
    memcpy(&t0->ctxt[SYSCALL], &p->ctxt[SYSCALL], sizeof(CONTEXT));
    
    /* Thread0 uses process TSD data (shared) */
    t0->tsd_data = p->proc_tsd_data;
    
    /* Main thread is NOT cancellable (protect process main) */
    t0->cancel_state = PTHREAD_CANCEL_DISABLE;
    t0->cancel_type = PTHREAD_CANCEL_DEFERRED;
    
    t0->has_run = 1;
    /* Link into process (thread0 is the only thread initially) */
    p->threads = t0;
    p->current_thread = t0;
    p->num_threads = 1;
    p->total_threads = 1;
    
    /* Enable threaded signal handling for process */
    p->p_sigacts->thread_signals = 1;
    p->p_sigacts->flags |= SAS_THREADED;
    
    /* Set thread0 signal mask */
    THREAD_SIGMASK_SET(t0, p->p_sigmask);

    /* Mark thread as running */
    proc_thread_state_change(t0, THREAD_STATE_RUNNING);
    
    TRACE_THREAD("INIT CONTEXT: Thread0 initialized for process %d", p->pid);
    TRACE_THREAD(" CURRENT: ssp=%lx, usp=%lx, pc=%lx", 
                 t0->ctxt[CURRENT].ssp, t0->ctxt[CURRENT].usp, 
                 t0->ctxt[CURRENT].pc);
    TRACE_THREAD(" SYSCALL: ssp=%lx, usp=%lx, pc=%lx", 
                 t0->ctxt[SYSCALL].ssp, t0->ctxt[SYSCALL].usp, 
                 t0->ctxt[SYSCALL].pc);
    
    // /* Start thread timer for scheduling */
    TRACE_THREAD("INIT CONTEXT: Starting thread timer for process %d", p->pid);
    thread_timer_start(t0->proc, t0->tid);
}

/******************************************************************************/
/* handle_thread_mode_switching - Handle mode switching for thread ops      */
/******************************************************************************/
struct thread *handle_thread_mode_switching(struct proc *p) {
    if (!p->current_thread) {
        TRACE_THREAD("HANDLE SWITCH TO THREADED MODE: current thread is NULL");
        init_main_thread_context(curproc);
        TRACE_THREAD("HANDLE SWITCH TO THREADED MODE: Initialized main thread context for process %d", p->pid);
        p->p_flag &= ~P_FLAG_THREADED;
        TRACE_THREAD("HANDLE SWITCH TO THREADED MODE: Cleared P_FLAG_THREADED for process %d", p->pid);
        if (p->p_sigacts) {
            /* Disable thread signals for forked child */
            // p->p_sigacts->thread_signals = 0;
            p->p_sigacts->flags &= ~SAS_THREADED;
            TRACE_THREAD("HANDLE SWITCH TO THREADED MODE: Disabled thread signals in process PID %d", p->pid);
        }
        if(p->current_thread) {
            return p->current_thread;
        }
        return NULL;
    }
    return p->current_thread;
}

/******************************************************************************/
/* get_thread_context - Return appropriate context for thread                */
/* Returns signal context if handling signal, otherwise syscall context      */
/******************************************************************************/
CONTEXT* get_thread_context(struct thread *t) {
    /* Validate thread */
    if (!t || t->magic != CTXT_MAGIC || (t->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("GET_CTX ERROR: Invalid thread reference");
        return NULL;
    }

    /* If handling signal, return signal context */
    if (t->t_sig_in_progress) {
        TRACE_THREAD("GET_CTX: Using signal context for thread %d", t->tid);
        return &t->sig_ctx;
    }

    TRACE_THREAD("GET_CTX: CURRENT context for thread %d, SR=%x, PC=%lx, "
                 "SSP=%lx, USP=%lx", 
                 t->tid, t->ctxt[CURRENT].sr, t->ctxt[CURRENT].pc, 
                 t->ctxt[CURRENT].ssp, t->ctxt[CURRENT].usp);
    TRACE_THREAD("GET_CTX: SYSCALL context for thread %d, SR=%x, PC=%lx, "
                 "SSP=%lx, USP=%lx", 
                 t->tid, t->ctxt[SYSCALL].sr, t->ctxt[SYSCALL].pc, 
                 t->ctxt[SYSCALL].ssp, t->ctxt[SYSCALL].usp);
    
    /* Return syscall context (standard case) */
    return &t->ctxt[SYSCALL];
}

/******************************************************************************/
/* proc_thread_cleanup_process - Clean up all threads when process exits     */
/* Ensures proper cleanup order to avoid race conditions                     */
/******************************************************************************/
void proc_thread_cleanup_process(struct proc *pcurproc) {
    struct thread *t, *next;
    TIMEOUT *timelist, *next_timelist;
    
    if (!pcurproc->threads) return;
    
    TRACE_THREAD("PROC THREAD CLEANUP PROCESS: cleaning up threads for pid=%d", pcurproc->pid);

    /* Step 1: Stop thread timer FIRST to prevent scheduling during cleanup */
    if (pcurproc->p_thread_timer.enabled) {
        TRACE_THREAD("PROC THREAD CLEANUP PROCESS: stopping thread timer");
        thread_timer_stop(pcurproc);
    }

    /* Step 2: Cancel all timeouts for this process */
    for (timelist = tlist; timelist; timelist = next_timelist) {
        next_timelist = timelist->next;
        if (timelist->proc == pcurproc) {
            TRACE_THREAD("PROC THREAD CLEANUP PROCESS: cancelling timeout for pid=%d", pcurproc->pid);
            canceltimeout(timelist);
        }
    }

    /* Step 3: Remove all threads from queues BEFORE clearing sync states */
    for (t = pcurproc->threads; t; t = t->next) {
        if (t->magic == CTXT_MAGIC) {
            TRACE(("PROC THREAD CLEANUP PROCESS: removing thread %d from queues", t->tid));
            remove_thread_from_wait_queues(t);
            remove_from_ready_queue(t);
            /* Mark as exited but don't free yet */
            t->state |= THREAD_STATE_EXITED;
        }
    }
    
    /* Step 4: Clean up idle thread explicitly */
    if (pcurproc->idle_thread) {
        TRACE(("PROC THREAD CLEANUP PROCESS: cleaning up idle thread"));
        cleanup_thread_resources(pcurproc, pcurproc->idle_thread, 
                                pcurproc->idle_thread->tid);
        pcurproc->idle_thread = NULL;
    }
    
    /* Step 5: Clean up sync states (mutexes, semaphores, etc.) */
    TRACE(("PROC THREAD CLEANUP PROCESS: cleaning up thread sync states"));
    cleanup_thread_sync_states(pcurproc);

    /* Step 6: Free individual thread resources */
    t = pcurproc->threads;
    while (t) {
        next = t->next;
        
        if (t->magic == CTXT_MAGIC && t->tid != 0) {
            TRACE(("terminate: freeing thread %d resources", t->tid));
            cleanup_thread_resources(pcurproc, t, t->tid);
        }
        
        t = next;
    }

    /* Step 7: Clean up process-wide thread state */
    TRACE(("PROC THREAD CLEANUP PROCESS: clearing process thread state"));
    pcurproc->current_thread = NULL;
    pcurproc->num_threads = 0;
    pcurproc->total_threads = 0;
    pcurproc->threads = NULL;

    /* Step 8: Clean up process-wide TSD (last step) */
    TRACE(("PROC THREAD CLEANUP PROCESS: cleaning up TSD"));
    cleanup_proc_tsd(pcurproc);
}

/******************************************************************************/
/* proc_thread_status - Get current status of a thread                       */
/******************************************************************************/
long proc_thread_status(long tid) {
    struct proc *p = curproc;
    struct thread *target = NULL;
    register unsigned short sr;
    long status;
    
    if (!p) {
        return EINVAL;
    }
    
    /* Find target thread - protect with splhigh */
    sr = splhigh();
    
    for (target = p->threads; target; target = target->next) {
        if (target->tid == tid) {
            break;
        }
    }
    
    if (!target) {
        spl(sr);
        TRACE_THREAD("STATUS: No such thread %d", tid);
        return ESRCH;  /* Thread not found */
    }
    
    status = target->state;
    
    spl(sr);
    
    return status;
}

/******************************************************************************/
/* idle_thread_func - Idle thread main loop                                  */
/* Runs when no other threads are ready                                      */
/******************************************************************************/
static void *idle_thread_func(void *arg) {
    struct proc *p = (struct proc *)arg;
    
    if (!p || p->magic != CTXT_MAGIC) {
        TRACE_THREAD("IDLE: Invalid process pointer");
        return NULL;
    }
    
    /* Lower process priority for idle thread */
    p->pri = p->pri + 1;

    /* Idle loop - yield CPU repeatedly */
    while (1) {
        kernel_pthread_syscall(P_THREAD_SYNC, THREAD_SYNC_YIELD, 0, 0);
    }

    /* Restore original process priority (never reached) */
    p->pri = p->pri - 1;

    return NULL;
}

/******************************************************************************/
/* create_idle_thread - Create idle thread for process                       */
/* Idle thread runs at lowest priority when no other threads are ready       */
/******************************************************************************/
static struct thread* create_idle_thread(struct proc *p) {
    struct thread *idle;
    
    /* Allocate idle thread structure */
    idle = kmalloc(sizeof(struct thread));
    if (!idle) {
        return NULL;
    }
    
    /* Zero entire structure */
    mint_bzero(idle, sizeof(*idle));
    
    /* Allocate stack for idle thread */
    idle->stack = kmalloc(STKSIZE);
    if (!idle->stack) {
        kfree(idle);
        return NULL;
    }
    
    /* === Set ONLY non-zero fields === */
    
    /* Idle thread identity (negative tid indicates special thread) */
    idle->tid = -128;
    idle->proc = p;
    idle->magic = CTXT_MAGIC;
    idle->is_idle = 1;
    strncpy(idle->name, "idle", 15);
    idle->name[15] = '\0';
    
    /* Stack setup */
    idle->stack_size = STKSIZE;
    idle->stack_top = (char*)idle->stack + STKSIZE;
    idle->stack_magic = STACK_MAGIC;
    
    /* Lowest possible priority (below all user threads) */
    idle->priority = MIN_THREAD_PRIORITY;
    idle->original_priority = idle->priority;
    idle->policy = DEFAULT_SCHED_POLICY;
    
    /* Scheduling timeslice */
    idle->timeslice = p->thread_default_timeslice;
    idle->remaining_timeslice = p->thread_default_timeslice;
    
    /* Inherit process signal mask */
    THREAD_SIGMASK_SET(idle, p->p_sigmask);
    
    /* Idle thread is detached and not cancellable */
    idle->detached = 1;
    idle->cancel_state = PTHREAD_CANCEL_DISABLE;
    idle->cancel_type = PTHREAD_CANCEL_DEFERRED;
    
    /* Initialize context */
    init_thread_context(idle, idle_thread_func, (void *)p);
    
    /* Link into process thread list (at head) */
    idle->next = p->threads;
    p->threads = idle;
    
    /* Set as process idle thread */
    p->idle_thread = idle;
    
    /* Make ready to run */
    proc_thread_state_change(idle, THREAD_STATE_READY);
    add_to_ready_queue(idle);

    TRACE_THREAD("IDLE: Created idle thread with tid %d", idle->tid);

    return idle;
}

/******************************************************************************/
/* get_idle_thread - Get or create idle thread for process                   */
/******************************************************************************/
struct thread* get_idle_thread(struct proc *p) {
    if (!p) {
        return NULL;
    }
    
    /* Create idle thread if it doesn't exist yet */
    if (!p->idle_thread) {
        return create_idle_thread(p);
    }
    
    return p->idle_thread;
}

/******************************************************************************/
/* get_main_thread - Get main thread (thread0) for process                   */
/******************************************************************************/
struct thread* get_main_thread(struct proc *p) {
    struct thread *t;
    
    /* Search for thread with tid=0 */
    for (t = p->threads; t != NULL; t = t->next) {
        if (t->tid == 0 && t->magic == CTXT_MAGIC && 
            !(t->state & THREAD_STATE_EXITED)) {
            return t;
        }
    }
    
    return NULL;
}
