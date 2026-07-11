/**
 * @file proc_threads_signal.c
 * @brief Kernel Thread Signal Handling
 * 
 * Implements signal management for threaded processes within the kernel.
 * Handles per-thread signal masking, targeted delivery, and sigwait operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

 /**
 * Thread Signal Handling
 * 
 * Implements POSIX-compliant thread signal management with per-thread signal
 * masks, queuing, and handlers. Supports targeted signal delivery, sigwait,
 * and real-time signal management in threaded environments.
 */

#include "proc_threads_signal.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_cancel.h"
#include "arch/kernel.h"
#include "signal.h"

#define MIN_THREAD_RUNTIME_TICKS 10  /* Minimum ticks a thread must have run before receiving signals */

/* Thread timeout handling */
static void thread_timeout_sighandler(PROC *p, long arg);

/* Forward declarations */
static void thread_signal_alarm_handler(PROC *p, long arg);

/* Trampoline function to call thread signal handlers with proper context management */
static void thread_signal_trampoline(int sig, struct thread *t);
// static void handler_execute(int sig, void *arg);
static void cleanup_thread_sigqueue(struct thread *t);

static int dequeue_thread_signal_info(struct thread *t, const sigset_t *set, siginfo_t *info);
static int enqueue_thread_signal_info(struct thread *t, int sig, const siginfo_t *info);
/* This function must be in user-accessible memory or mapped appropriately */
static void signal_handler_return(void)
{
    /* This is called when the signal handler returns */
    /* Make a syscall to restore the original context */
    
    /* Use the pthread syscall mechanism */
    asm volatile (
        "movl   #0, %%sp@-\n\t"           /* arg2 = 0 */
        "movl   #0, %%sp@-\n\t"           /* arg1 = 0 */
        "movl   #19, %%sp@-\n\t"          /* op = THREAD_CTRL_SIGRETURN */
        "movl   #1, %%sp@-\n\t"           /* subsystem = P_THREAD_CTRL */
        "movw   #0x185, %%sp@-\n\t"       /* P_PTHREAD */
        "trap   #1\n\t"
        "lea    %%sp@(18), %%sp"
        :
        :
        : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory"
    );
    
    /* Should never return here */
    while(1);
}

/**
 * Timeout handler for sleeping threads.
 *
 * This function is called when a thread sleeping timeout expires.
 * It will wake up the thread and put it back into the ready queue,
 * unless the thread is not sleeping or is not blocked.
 *
 * @param p: the process containing the thread
 * @param arg: ignored
 */
static void thread_timeout_sighandler(PROC *p, long arg)
{
    struct thread *t = (struct thread *)arg;
    TRACE_THREAD("thread_timeout_sighandler: thread %d timeout", t ? t->tid : -1);
    if(!t) return;

    if ((t->state & THREAD_STATE_BLOCKED) && (t->wait_type & WAIT_SIGNAL)) {
        t->sleep_reason = 1; // Timeout

        // Remove from any wait queue if present
        remove_thread_from_wait_queues(t);

        // Clear wait type
        TRACE_THREAD("TIMEOUT: Waking thread %d from timeout - removing WAIT_SIGNAL flag", t->tid);
        t->wait_type &= ~WAIT_SIGNAL;
        
        // Add to ready queue
        proc_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);
    }

}

/*
 * Signal trampoline function to set up signal handler execution in USER MODE
 * 
 * This function prepares the thread context to execute the signal handler
 * as user code. When the handler returns, it will call signal_handler_return
 * which makes a syscall back to the kernel to restore the original context.
 */
static void thread_signal_trampoline(int sig, struct thread *t)
{
    struct proc *p = t->proc;

    TRACE_THREAD("SIGNAL TRAMPOLINE - thread_signal_trampoline: thread %d signal %d", 
                 t ? t->tid : -1, sig);
    
    if (!t || !p || !p->p_sigacts || sig <= 0 || sig >= NSIG)
        return;
        
    /* Clean up any previous signal stack */
    cleanup_signal_stack(p, (long)t);

    /* Get handler from thread-specific storage */
    void (*handler)(int, void*) = t->sig_handlers[sig].handler;
    void *handler_arg = t->sig_handlers[sig].arg;
    
    if (!handler) {
        TRACE_THREAD("SIGNAL TRAMPOLINE: No handler for signal %d", sig);
        return;
    }

    TRACE_THREAD("SIGNAL TRAMPOLINE: Handler=%p, arg=%p for signal %d", 
                 handler, handler_arg, sig);

    /* Save the old signal mask */
    t->old_sigmask = THREAD_SIGMASK(t);
    
    /* Add this signal to the mask to prevent recursive handling */
    THREAD_SIGMASK_ADD_SIGNAL(t, sig);
    
    /* Allocate a new stack for the signal handler */
    t->sig_stack = kmalloc(STKSIZE);
    if (!t->sig_stack) {
        TRACE_THREAD("SIGNAL TRAMPOLINE: Failed to allocate signal stack for thread %d", t->tid);
        return;
    }

    /* Mark that we're processing a signal */
    t->t_sig_in_progress = sig;

    /* Save the ORIGINAL context so we can restore it after signal handler completes */
    memcpy(&t->saved_ctx, &t->ctxt[SYSCALL], sizeof(CONTEXT));
    /* Initialize sig_ctx from current context */
    memcpy(&t->sig_ctx, &t->ctxt[SYSCALL], sizeof(CONTEXT));
    
    TRACE_THREAD("SIGNAL TRAMPOLINE: Saved original context - PC=%lx, SSP=%lx, USP=%lx, SR=%x", 
                 t->saved_ctx.pc, t->saved_ctx.ssp, t->saved_ctx.usp, t->saved_ctx.sr);

    /* Set up NEW user-mode stack for signal handler */
    unsigned long sig_usp = ((unsigned long)t->sig_stack + STKSIZE - 512) & ~3L;
    unsigned long sig_ssp = ((unsigned long)t->sig_stack + STKSIZE - 256) & ~3L;
    
    /* Configure signal context to execute handler in USER MODE */
    t->sig_ctx.usp = sig_usp;
    t->sig_ctx.ssp = sig_ssp;
    t->sig_ctx.pc = (unsigned long)handler;
    t->sig_ctx.sr = 0x0000;

    /* Set up registers for the function call */
    t->sig_ctx.regs[0] = sig;                    /* D0 = signal number */
    t->sig_ctx.regs[1] = (unsigned long)handler_arg; /* D1 = handler argument */

    /* M68K calling convention: parameters pushed right-to-left on stack */
    /* Stack grows downward, so we push return address first, then parameters */
    
    unsigned long *stack = (unsigned long *)sig_usp;
    
    /* Push parameters (right to left for C calling convention) */
    if (handler_arg != NULL) {
        /* Thread handler: void (*)(int sig, void *arg) */
        stack--;
        *stack = (unsigned long)handler_arg;  // Second parameter: arg
    }
    
    stack--;
    *stack = (unsigned long)sig;  // First parameter: signal number
    
    stack--;
    *stack = (unsigned long)signal_handler_return;  // Return address
    
    t->sig_ctx.usp = (unsigned long)stack;
        
    TRACE_THREAD("SIGNAL TRAMPOLINE: Signal context prepared for thread %d", t->tid);
    TRACE_THREAD("  Handler PC=%lx (USER MODE), SR=%x", t->sig_ctx.pc, t->sig_ctx.sr);
    TRACE_THREAD("  SSP=%lx, USP=%lx", t->sig_ctx.ssp, t->sig_ctx.usp);
    TRACE_THREAD("  D0=%lx (sig=%d), D1=%lx (arg=%p)", 
                 t->sig_ctx.regs[0], sig, t->sig_ctx.regs[1], handler_arg);
    TRACE_THREAD("  Return address=%lx (signal_handler_return)", 
                 (unsigned long)signal_handler_return);
    
    /* Copy signal context to SYSCALL context 
     * This makes the signal handler execute when we return from the current syscall */
    memcpy(&t->ctxt[SYSCALL], &t->sig_ctx, sizeof(CONTEXT));

    boost_thread_priority(t, 5);
    
    TRACE_THREAD("SIGNAL TRAMPOLINE: Updated SYSCALL context - will execute handler on syscall return");
    TRACE_THREAD("  SYSCALL context now: PC=%lx, SR=%x, USP=%lx, SSP=%lx",
                 t->ctxt[SYSCALL].pc, t->ctxt[SYSCALL].sr, 
                 t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].ssp);
    return;
}

/*
 * Process thread signal
 * WARNING: For now Non USER_SIGNAL always go to process level
 */
void handle_thread_signal(struct thread *t, int sig) {
    if (!t || sig <= 0 || sig >= NSIG) {
        TRACE_THREAD("HANDLE THREAD SIGNAL: ERROR - Invalid thread or invalid parameters");
        CLEAR_THREAD_SIGPENDING(t, 0);
        return;
    }

    /* Non-thread signals always go to process level */
    if (!IS_THREAD_USER_SIGNAL(sig)) {
        CLEAR_THREAD_SIGPENDING(t, sig);
        struct proc *p = t->proc;
        p->sigpending |= (1UL << sig);
        return;
    }
    
    /* Thread 0 CAN handle thread-specific signals (SIGUSR1/SIGUSR2)
     * using the trampoline, just like other threads! Don't redirect to process level!
     */
    
    if (t->t_sig_in_progress || !t->has_run) {
        TRACE_THREAD("HANDLE THREAD SIGNAL: WARNING - Thread %d already handling signal %d, function called for sig %d or thread %d has not yet run", t->tid, t->t_sig_in_progress, sig, t->tid);
        return;
    }

    /* Thread-specific handlers only for SIGUSR1/SIGUSR2 */
    void (*thread_handler)(int, void*) = NULL;
    if (IS_THREAD_USER_SIGNAL(sig)) {
        thread_handler = t->sig_handlers[sig].handler;
    }
    
    if (thread_handler) {
        TRACE_THREAD("HANDLE THREAD SIGNAL: INFO - using thread-specific handler for signal %d", sig);
        CLEAR_THREAD_SIGPENDING(t, sig);
        thread_signal_trampoline(sig, t);
        return;
    }

    /* FALL BACK to process handler */ 
    struct proc *p = t->proc;
    if (p && p->p_sigacts) {
        struct sigaction *sigact = &SIGACTION(p, sig);

        /* CRITICAL FIX: Apply SA_RESETHAND immediately before any dispatch */
        if (sigact->sa_flags & SA_RESETHAND) {
            sigact->sa_handler = SIG_DFL;
            sigact->sa_flags &= ~SA_RESETHAND;
            if (p->p_sigacts->sa_sigaction_ext[sig]) {
                p->p_sigacts->sa_sigaction_ext[sig] = NULL;
            }
        }
    
        /* Now check disposition (may have just been reset above) */
        if (sigact->sa_handler == SIG_DFL) {
            TRACE_THREAD("HANDLE THREAD SIGNAL: SIG_DFL for signal %d, routing to process", sig);
            CLEAR_THREAD_SIGPENDING(t, sig);
            p->sigpending |= (1UL << sig);
        }

        if (sigact->sa_handler != SIG_DFL && sigact->sa_handler != SIG_IGN ) {
            TRACE_THREAD("HANDLE THREAD SIGNAL: using process handler for signal %d", sig);
            CLEAR_THREAD_SIGPENDING(t, sig);
            
            t->sig_handlers[sig].handler = (void (*)(int,  void *))sigact->sa_handler;
            t->sig_handlers[sig].arg = NULL;

            thread_signal_trampoline(sig, t);
            
            t->sig_handlers[sig].handler = NULL;
            t->sig_handlers[sig].arg = NULL;

            return;
        }
    }
    
    /* No handler for SIGUSR1/SIGUSR2 */
    
    /* Case 1: Thread in sigwait - keep signal pending */
    if (t->wait_type & WAIT_SIGNAL) {
        TRACE_THREAD("HANDLE THREAD SIGNAL: thread %d in sigwait, keeping signal %d pending", 
                     t->tid, sig);
        return;
    }
    
    /* Case 2: Grace period for handler installation */
    if (!t->last_scheduled) {
        TRACE_THREAD("HANDLE THREAD SIGNAL: user signal %d, thread just started, brief grace period, last scheduled not set", sig);
        return;
    }

    /* Case 3: No handler - default is to ignore SIGUSR1/SIGUSR2 */
    TRACE_THREAD("HANDLE THREAD SIGNAL: no handler for user signal %d, ignoring", sig);
    CLEAR_THREAD_SIGPENDING(t, sig);
    return;
}

/*
 * Deliver a signal to a specific thread
 * Returns 1 if signal was delivered, 0 otherwise
 */
int deliver_signal_to_thread(struct proc *p, struct thread *t, int sig, const siginfo_t *info)
{
    unsigned short sr;

    if (!p || !t || sig <= 0 || sig >= NSIG) {
        TRACE_THREAD("ERROR - DELIVER SIGNAL TO THREAD: Invalid parameters sig=%d", sig);
        return 0;
    }

    if (t->proc != p) {
        TRACE_THREAD("ERROR - DELIVER SIGNAL TO THREAD: Thread %d in wrong process", t->tid);
        return 0;
    }

    /* Check if thread-specific signal handling is enabled */
    if (!p->p_sigacts || !p->p_sigacts->thread_signals) {
        TRACE_THREAD("ERROR - DELIVER SIGNAL TO THREAD: Thread signals not enabled");
        return 0;
    }

    /* ============================================================
     * Check for sigwait() BEFORE checking if blocked
     * This allows blocked signals to be delivered to threads waiting
     * in sigwait(), which is the whole point of sigwait()!
     * ============================================================ */

    /* Is this thread waiting in sigwait()? */
    if ((t->state & THREAD_STATE_BLOCKED) &&
        (t->wait_type & WAIT_SIGNAL) &&
        t->sig_wait_obj) {

        ulong wait_mask = (ulong)t->sig_wait_obj;

        /* Does this signal match what the thread is waiting for? */
        if (wait_mask & (1UL << sig)) {
            TRACE_THREAD("DELIVER: Signal %d matches sigwait mask for thread %d, waking",
                         sig, t->tid);

            /* Mark signal as pending - sigwait will consume it */
            SET_THREAD_SIGPENDING(t, sig);

            /* If siginfo provided, try to enqueue it */
            if (info && info->si_code == SI_QUEUE) {
                enqueue_thread_signal_info(t, sig, info);
            }

            /* Wake the thread from sigwait() */
            TRACE_THREAD("DELIVER: Waking thread %d from sigwait() - removing WAIT_SIGNAL flag and sig_wait_obj", t->tid);
            sr = splhigh();
            t->wait_type &= ~WAIT_SIGNAL;
            t->sig_wait_obj = NULL;
            t->sleep_reason = 0;
            spl(sr);

            /* Remove from signal wait queue using helper */
            remove_thread_from_specific_wait_queue(t, WAIT_SIGNAL);  

            /* Make thread ready to run */
            proc_thread_state_change(t, THREAD_STATE_READY);
            if (!is_in_ready_queue(t)) {
                add_to_ready_queue(t);
            }

            TRACE_THREAD("DELIVER: Thread %d woken from sigwait(), signal %d pending",
                         t->tid, sig);

            return 1; /* Success - signal delivered to waiting thread */
        }
        TRACE_THREAD("DELIVER: Signal %d does not match sigwait mask for thread %d",
                     sig, t->tid);
    }

    /* Wake thread if sleeping (signal should interrupt sleep) */
    if ((t->state & THREAD_STATE_BLOCKED) && 
                (t->wait_type & WAIT_SLEEP)) {
        SET_THREAD_SIGPENDING(t, sig);
        TRACE_THREAD("DELIVER: Signal %d pending for sleeping thread %d, waking immediately", sig, t->tid);
        return 1;
    }

    /* Now check if signal is blocked (only if NOT in sigwait) */
    if (THREAD_SIGMASK(t) & (1UL << sig)) {
        TRACE_THREAD("ERROR - DELIVER SIGNAL TO THREAD: Signal %d blocked by thread %d",
                     sig, t->tid);
        /* Still mark as pending so it can be caught when unmasked */
        SET_THREAD_SIGPENDING(t, sig);
        return 0;
    }

    TRACE_THREAD("DELIVER SIGNAL TO THREAD: Trying to Deliver signal %d to thread %d", sig, t->tid);

    /* If siginfo provided, enqueue it (prefer detailed queue) */
    if (info && info->si_code == SI_QUEUE) {
        enqueue_thread_signal_info(t, sig, info);
    }

    /* Mark signal as pending */
    SET_THREAD_SIGPENDING(t, sig);
    
    /* PRIORITY 2: Check for handlers (only if NOT in sigwait) */
    void (*thread_handler)(int, void*) = t->sig_handlers[sig].handler;
    struct sigaction *proc_sigaction = &SIGACTION(p, sig);
    void (*proc_handler)(int) = (void (*)(int))proc_sigaction->sa_handler;

    int has_handler = (thread_handler != NULL) ||
                      (proc_handler != (void*)SIG_DFL &&
                       proc_handler != (void*)SIG_IGN &&
                       proc_handler != NULL);

    if (!has_handler) {
        TRACE_THREAD("DELIVER SIGNAL: No handler for signal %d, leaving thread %d blocked",
                     sig, t->tid);
        return 1;
    }
    /* Wake thread if blocked so handler can run */
    if (t->state & THREAD_STATE_BLOCKED) {
        proc_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);
    }
    return 1;
}

/*
 * Modified version of raise() that's thread-aware
 */
int proc_thread_signal_aware_raise(struct proc *p, int sig)
{
    struct thread *t = CURTHREAD;
    
    if (!p || sig < 1 || sig >= NSIG)
        return EINVAL;
        
    /* ========== ONLY ROUTE THREAD-SPECIFIC SIGNALS TO THREADS ========== */


    /* Check if this is targeted at a SPECIFIC thread (tid > 0) */
    /* AND thread-specific handling is enabled */
    if (p->p_sigacts && 
        p->p_sigacts->thread_signals && 
        t && 
        // t->tid > 0 && 
        IS_THREAD_USER_SIGNAL(sig)
    ) {

        TRACE_THREAD("THREAD RAISE: Attempting thread-aware delivery for signal %d", sig);

        short delivered = 0;

        if (t->tid == 0) goto process_level_delivery;
        
        TRACE_THREAD("THREAD RAISE: Trying current thread %d first", 
                    p->current_thread->tid);
        
        /* Try to deliver to current thread (works for tid 0 too!) */
        if (deliver_signal_to_thread(p, p->current_thread, sig, NULL)) {
            TRACE_THREAD("THREAD RAISE: Signal %d delivered to current thread %d", 
                        sig, p->current_thread->tid);
            return 0;
        }
        
        TRACE_THREAD("THREAD RAISE: Current thread delivery failed (blocked or no handler?)");

        /* ==================================================================== */        
        /* First pass: prioritize threads explicitly waiting for this signal in sigwait */
        for (t = p->threads; t != NULL; t = t->next) {
            // if (t->is_idle || t == p->current_thread || t->tid == 0) continue;
            if (t->is_idle || t == p->current_thread) continue;
            
            if ((t->state & THREAD_STATE_BLOCKED) && 
                (t->wait_type & WAIT_SIGNAL) && 
                t->sig_wait_obj) {
                ulong wait_mask = (ulong)t->sig_wait_obj;
                if (wait_mask & (1UL << sig)) {
                    TRACE_THREAD("THREAD RAISE: Thread %d is waiting for signal %d in sigwait", 
                                t->tid, sig);
                    if (deliver_signal_to_thread(p, t, sig, NULL)) {
                        delivered = 1;
                        TRACE_THREAD("THREAD RAISE: Signal %d delivered to waiting thread %d", 
                                    sig, t->tid);
                        break;
                    }
                }
            }
        }
        
        /* Second pass: try other eligible threads */
        if (!delivered) {
            for (t = p->threads; t != NULL; t = t->next) {
                if (t->is_idle || t == p->current_thread || t->tid == 0) continue;
                delivered |= deliver_signal_to_thread(p, t, sig, NULL);
            }
        }
        
        if (delivered) {
            TRACE_THREAD("THREAD RAISE: Signal %d delivered to a thread", sig);
            return 0;
        }
    }
    /* ==================================================================== */
process_level_delivery:
    /* Default: deliver to process (for all process-level signals or if thread delivery failed) */
    TRACE_THREAD("THREAD RAISE: Delivering signal %d to process level", sig);
    p->sigpending |= (1UL << sig);
    
    return 0;
}

/*
 * Check for pending signals in a thread
 * Returns signal number if a signal is pending, 0 otherwise
 */
int check_thread_signals(struct thread *t)
{
    
    if (!t){
        TRACE_THREAD("CHECK THREAD SIGNALS: invalid thread pointer");
        return 0;
    }

    int sig;
    ulong pending = (THREAD_SIGPENDING(t) & ~THREAD_SIGMASK(t)) & ~1UL;
     
    /* CRITICAL: Clear bit 0 if somehow set */
    t->t_sigpending &= ~1UL;
    
    if (!pending){
        TRACE_THREAD("CHECK THREAD SIGNALS: no pending signals for thread %d, sigmask %lx", t->tid, THREAD_SIGMASK(t));
        return 0;
    }

    TRACE_THREAD("CHECK THREAD SIGNALS: pending signals %lx for thread %d, sigmask %lx", 
                 pending, t->tid, THREAD_SIGMASK(t));

    /* Find the first pending signal */
    for (sig = 1; sig < NSIG; sig++) {
        if (pending & (1UL << sig)) {
            /* DON'T clear the pending flag here - it will be cleared after handling */
            TRACE_THREAD("CHECK THREAD SIGNALS: thread %d has pending signal %d", t->tid, sig);
            return sig;
        }
    }

    TRACE_THREAD("CHECK THREAD SIGNALS: no valid pending signals for thread %d", t->tid);
    return 0;
}

/*
 * Thread alarm handler
 */
static void thread_signal_alarm_handler(PROC *p, long arg)
{
    struct thread *t = (struct thread *)arg;
    
    if (!t || !p) {
        TRACE_THREAD(("THREAD SIGNAL ALARM HANDLER: invalid thread or process"));
        return;
    }

    /* Verify thread belongs to this process */
    if (t->proc != p) {
        TRACE_THREAD("THREAD SIGNAL ALARM HANDLER: thread %d belongs to different process", t->tid);
        return;
    }

    /* CRITICAL: Verify thread is still valid before accessing */
    if (!t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("THREAD SIGNAL ALARM HANDLER: invalid thread");
        return;
    }
    
    /* Verify thread is still alive */
    if (t->state & THREAD_STATE_EXITED) {
        TRACE_THREAD("THREAD SIGNAL ALARM HANDLER: thread %d has exited", t->tid);
        t->alarm_timeout = NULL;
        return;
    }

    /* Clear the thread's alarm timeout */
    t->alarm_timeout = NULL;

    /* Verify thread is still valid and not being destroyed */
    if (t->state == THREAD_STATE_ZOMBIE) {
        TRACE_THREAD("THREAD SIGNAL ALARM HANDLER: thread %d is zombie, ignoring alarm", t->tid);
        return;
    }

    /* Deliver SIGALRM specifically to this thread */
    deliver_signal_to_thread(p, t, SIGALRM, NULL);
    return;
}

/*
 * Enable/disable thread-specific signal handling for a process
 */
long _cdecl proc_thread_signal_mode(int enable)
{
    if (!curproc || !curproc->p_sigacts){
        TRACE_THREAD("THREAD SIGNAL MODE: invalid process");
        return EINVAL;
    }

    if (enable) {
        TRACE_THREAD("THREAD SIGNAL MODE: enabling thread signals");
        curproc->p_sigacts->thread_signals = 1;
        curproc->p_sigacts->flags |= SAS_THREADED;
    } else {
        TRACE_THREAD("THREAD SIGNAL MODE: disabling thread signals");
        curproc->p_sigacts->thread_signals = 0;
        curproc->p_sigacts->flags &= ~SAS_THREADED;
    }
        
    return 0;
}

/*
 * Set signal mask for current thread
 */
long _cdecl proc_thread_signal_sigmask(ulong mask)
{
    struct thread *t = CURTHREAD;
    int sig;
    
    if (!t) return EINVAL;    

    TRACE_THREAD("proc_thread_signal_sigmask: TID=%d, mask=0x%lx, current_mask=0x%lx, thread_signals=%d",
                 t->tid, mask, THREAD_SIGMASK(t), 
                 t->proc->p_sigacts ? t->proc->p_sigacts->thread_signals : -1);

    if (curproc->current_thread->tid == 0) {
        TRACE_THREAD("proc_thread_signal_sigmask: thread0 - setting process mask to 0x%lx", mask);        
        /* Process-level signal mask */
        curproc->p_sigmask = mask & ~(UNMASKABLE | 1UL);
        THREAD_SIGMASK_SET(curproc->current_thread, mask);
        
        // Sync thread0's pending signals to process level
        curproc->sigpending |= curproc->current_thread->t_sigpending;

        return 0;
    }
    
    /* Directly set new mask while excluding unmaskable signals */
    THREAD_SIGMASK_SET(curproc->current_thread, mask);

    TRACE_THREAD("proc_thread_signal_sigmask: set thread mask to 0x%lx", mask);

    /* Dispatch any signals that are now unmasked AND have handlers */
    ulong unmasked_pending = THREAD_SIGPENDING(curproc->current_thread) & ~mask;
    if (unmasked_pending) {
        TRACE_THREAD("proc_thread_signal_sigmask: unmasked_pending=0x%lx", unmasked_pending);
        for (sig = 1; sig < NSIG; sig++) {
            if ((unmasked_pending & (1UL << sig)) && 
                curproc->current_thread->sig_handlers[sig].handler) {
                TRACE_THREAD("proc_thread_signal_sigmask: dispatching signal %d / Calling handle_thread_signal()", sig);
                handle_thread_signal(curproc->current_thread, sig);
            }
        }
    }
    return 0;
}

/*
 * Send a signal to a specific thread
 */
long _cdecl proc_thread_signal_kill(struct thread *t, int sig)
{
    struct proc *p;
    
    /* Validate parameters */
    if (!t || sig < 0 || sig >= NSIG) {
        TRACE_THREAD("THREAD SIGNAL KILL: invalid parameters (t=%p, sig=%d)", t, sig);
        return EINVAL;
    }

    /* Thorough validation */
    if (t->state == THREAD_STATE_ZOMBIE || t->state == THREAD_STATE_EXITED || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("THREAD SIGNAL KILL: thread %d is not valid", t->tid);
        return ESRCH;
    }

    /* POSIX: pthread_kill(tid, 0) tests thread existence but doesn't deliver */
    if (sig == 0) {
        /* Thread exists and is valid - just return success */
        TRACE_THREAD("THREAD SIGNAL KILL: sig=0 existence check for thread %d - OK", t->tid);
        return 0;
    }

    p = t->proc;

    if (!p || !p->p_sigacts) {
        TRACE_THREAD("THREAD SIGNAL KILL: invalid process");
        return EINVAL;
    }

    /* Check if thread belongs to current process */
    if (p != curproc) {
        TRACE_THREAD("THREAD SIGNAL KILL: thread does not belong to current process");
        return EPERM;
    }

    /* Make sure thread-specific signals are enabled */
    if (!p->p_sigacts->thread_signals && !(t->tid == 0)) {
        TRACE_THREAD("THREAD SIGNAL KILL: thread-specific signals are disabled");
        return EINVAL;
    }

    if (t->tid == 0) {
        /* Thread 0 uses the classic FreeMiNT signal path for ALL signals.
        * This ensures proper signal queuing and delivery via check_sigs().
        * Signals are NOT delivered via thread trampoline for thread 0.
        */
        
        TRACE_THREAD("THREAD SIGNAL KILL: Using process-level delivery for thread 0, signal %d", sig);
        
        /* Use the classic process signal delivery */
        post_sig(p, sig);
        
        /* IMPORTANT: Do NOT call deliver_signal_to_thread() for thread 0!
        * post_sig() already handles waking the process if it's blocked.
        * The signal will be delivered via check_sigs() → handle_sig() → sendsig()
        * when the thread returns to user mode.
        */
        
        return 0;  /* Success */
    }

    /* Deliver signal to thread */
    if (!deliver_signal_to_thread(p, t, sig, NULL)){
        TRACE_THREAD("THREAD SIGNAL KILL: failed to deliver signal to thread");
        return EINVAL;
    }
    TRACE_THREAD("THREAD SIGNAL KILL: signal %d delivered to thread %d", sig, t->tid);
    return 0;
}

/*
 * Register a thread-specific signal handler
 */
long _cdecl proc_thread_signal_sighandler(int sig, void (*handler)(int, void*), void *arg)
{
    struct thread *t = CURTHREAD;
    struct proc *p = curproc;
    
    /* Validate parameters */
    if (!IS_THREAD_SIGNAL(sig)){
        TRACE_THREAD("THREAD SIGNAL HANDLER: invalid signal %d", sig);
        return EINVAL;
    }
        
    if (!t || !p || !p->p_sigacts){
        TRACE_THREAD("THREAD SIGNAL HANDLER: invalid process");
        return EINVAL;
    }
        
    /* Make sure thread-specific signals are enabled */
    if (!p->p_sigacts->thread_signals){
        TRACE_THREAD("THREAD SIGNAL HANDLER: thread-specific signals are disabled");
        return EINVAL;
    }

    /* Don't allow Idle Thread to set thread-specific handlers */
    if (t->is_idle) {
        TRACE_THREAD("THREAD SIGNAL HANDLER: idle thread cannot set thread-specific handlers");
        return EINVAL;
    }

    // /* Don't allow thread0 to set thread-specific handlers */
    // if (t->tid == 0 || t->is_idle) {
    //     TRACE_THREAD("THREAD SIGNAL HANDLER: thread0 or idle thread cannot set thread-specific handlers");
    //     return EINVAL;
    // }

    TRACE_THREAD("THREAD SIGNAL HANDLER: PROC ID %d, THREAD ID %d, SIG %d, HANDLER %p, ARG %p", p->pid, t->tid, sig, handler, arg);

    /* Store handler in thread-specific storage */
    t->sig_handlers[sig].handler = handler;
    t->sig_handlers[sig].arg = arg;

    /* Check for pending signals and dispatch immediately */
    if (handler && (THREAD_SIGPENDING(t) & (1UL << sig))) {
        TRACE_THREAD("THREAD SIGNAL HANDLER: signal %d pending, dispatching immediately / Calling handle_thread_signal()", sig);
        /* Dispatch the signal now that we have a handler */
        handle_thread_signal(t, sig);
    }

    return 0;
}

/*
 * Set the argument for a thread-specific signal handler
 */
long _cdecl proc_thread_signal_sighandler_arg(int sig, void *arg)
{
    struct thread *t = CURTHREAD;
    struct proc *p = curproc;
    
    /* Validate parameters */
    if (!IS_THREAD_SIGNAL(sig)){
        TRACE_THREAD("THREAD SIGNAL HANDLER ARG: invalid signal %d", sig);
        return EINVAL;
    }
        
    if (!t || !p || !p->p_sigacts){
        TRACE_THREAD("THREAD SIGNAL HANDLER ARG: invalid process");
        return EINVAL;
    }
        
    /* Make sure thread-specific signals are enabled */
    if (!p->p_sigacts->thread_signals){
        TRACE_THREAD("THREAD SIGNAL HANDLER ARG: thread-specific signals are disabled");
        return EINVAL;
    }
        
    /* Make sure a handler is already registered */
    if (!t->sig_handlers[sig].handler){
        TRACE_THREAD("THREAD SIGNAL HANDLER ARG: no handler registered for signal %d", sig);
        return EINVAL;
    }

    TRACE_THREAD("THREAD SIGNAL HANDLER ARG: PROC ID %d, THREAD ID %d, SIG %d, ARG %p", p->pid, t->tid, sig, arg);
    
    /* Set handler argument in thread-specific storage */
    t->sig_handlers[sig].arg = arg;
    
    return 0;
}

/*
 * Wait for signals with timeout
 */
long _cdecl proc_thread_signal_sigwait(ulong mask, long timeout)
{
    int sig, i;
    struct proc *p = curproc;
    struct thread *t = CURTHREAD;

    TIMEOUT *wait_timeout = NULL;
    siginfo_t info;
    sigset_t wait_set;

    if (!t || !p) {
        TRACE_THREAD("THREAD SIGNAL SIGWAIT: invalid thread or process");
        return EINVAL;
    }

    TRACE_THREAD("THREAD SIGNAL SIGWAIT: PROC ID %d, THREAD ID %d, mask %lx, timeout %ld",
                 p->pid, t->tid, mask, timeout);

    /* CRITICAL: Remove signal 0 from mask */
    mask &= ~1UL;

    /* Only allow thread signals (already excludes sig 0 now) */
    if (mask & ~THREAD_SIGNAL_MASK) {
        TRACE_THREAD("THREAD SIGNAL SIGWAIT: invalid mask");
        return EINVAL;
    }
    if (!mask) {
        TRACE_THREAD("THREAD SIGNAL SIGWAIT: empty mask");
        return EINVAL;
    }

    pthread_testcancel_internal(t);

    /* Save old mask */
    ulong old_mask = THREAD_SIGMASK(t);

    /* Unmask the signals we are waiting for */
    THREAD_SIGMASK_SET(t, old_mask & ~mask);

    /* ========== PRE-CHECK BEFORE SLEEP ========== */
    {
        sig = -1;

        /* 1) Thread queued signals */
        wait_set = mask;
        sig = dequeue_thread_signal_info(t, &wait_set, &info);
        if (sig > 0) {
            THREAD_SIGMASK_SET(t, old_mask);
            TRACE_THREAD("SIGWAIT: pre-check found thread queued sig %d", sig);
            return sig;
        }

        /* 2) Process queued signals */
        wait_set = mask;
        sig = dequeue_signal_info(p, t, &wait_set, &info);
        if (sig > 0) {
            THREAD_SIGMASK_SET(t, old_mask);
            TRACE_THREAD("SIGWAIT: pre-check found process queued sig %d", sig);
            return sig;
        }

        /* 3) Thread pending bits */
        for (i = 1; i < NSIG; i++) {
            if ((mask & (1ul << i)) && (THREAD_SIGPENDING(t) & (1ul << i))) {
                sig = i;
                CLEAR_THREAD_SIGPENDING(t, sig);
                break;
            }
        }
        if (sig > 0) {
            THREAD_SIGMASK_SET(t, old_mask);
            TRACE_THREAD("SIGWAIT: pre-check consumed pending sig %d", sig);
            return sig;
        }

        /* 4) Process pending bits */
        for (i = 1; i < NSIG; i++) {
            if ((mask & (1ul << i)) && (p->sigpending & (1ul << i))) {
                sig = i;
                p->sigpending &= ~(1ul << i);
                break;
            }
        }
        if (sig > 0) {
            THREAD_SIGMASK_SET(t, old_mask);
            TRACE_THREAD("SIGWAIT: pre-check consumed process pending sig %d", sig);
            return sig;
        }

    }
    /* ==================================================== */

    /* Setup timeout */
    if (timeout > 0) {
        wait_timeout = addtimeout(p, timeout, thread_timeout_sighandler);
        if (wait_timeout)
            wait_timeout->arg = (long)t;
    }

    /* Prepare to sleep */
    t->sig_wait_obj = (void*)mask;
    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    t->wait_type |= WAIT_SIGNAL;
    t->sleep_reason = 0;

    /* Add to wait queue */
    t->next_sigwait = NULL;
    if (p->signal_wait_queue) {
        struct thread *q = p->signal_wait_queue;
        while (q->next_sigwait)
            q = q->next_sigwait;
        q->next_sigwait = t;
    } else {
        p->signal_wait_queue = t;
    }

    remove_from_ready_queue(t);
    proc_thread_schedule();

    /* After wakeup */
    if (CURTHREAD != t) {
        TRACE_THREAD("SIGWAIT: wrong thread after wakeup");
        THREAD_SIGMASK_SET(t, old_mask);
        return EAGAIN;
    }

    pthread_testcancel_internal(t);

    if (wait_timeout) {
        canceltimeout(wait_timeout);
        wait_timeout = NULL;
    }

    /* Clear wait state */
    {
        unsigned short sr = splhigh();
        t->sig_wait_obj = NULL;

        /* Remove from signal wait queue */
        if (p->signal_wait_queue) {
            struct thread **tp = &p->signal_wait_queue;
            while (*tp) {
                if (*tp == t) {
                    *tp = t->next_sigwait;
                    t->next_sigwait = NULL;
                    break;
                }
                tp = &(*tp)->next_sigwait;
            }
        }
        spl(sr);
    }

    /* ========== POST-CHECK AFTER WAKEUP ========== */
    {
        sig = -1;

        /* 1) Thread queued */
        wait_set = mask;
        sig = dequeue_thread_signal_info(t, &wait_set, &info);
        if (sig > 0) {
            t->wait_type &= ~WAIT_SIGNAL;
            THREAD_SIGMASK_SET(t, old_mask);
            return sig;
        }

        /* 2) Process queued */
        wait_set = mask;
        sig = dequeue_signal_info(p, t, &wait_set, &info);
        if (sig > 0) {
            t->wait_type &= ~WAIT_SIGNAL;
            THREAD_SIGMASK_SET(t, old_mask);
            return sig;
        }

        /* 3) Thread pending bits */
        for (i = 1; i < NSIG; i++) {
            if ((mask & (1ul << i)) && (THREAD_SIGPENDING(t) & (1ul << i))) {
                sig = i;
                CLEAR_THREAD_SIGPENDING(t, sig);
                break;
            }
        }
        if (sig > 0) {
            t->wait_type &= ~WAIT_SIGNAL;
            THREAD_SIGMASK_SET(t, old_mask);
            return sig;
        }

        /* 4) Process pending bits */
        for (i = 1; i < NSIG; i++) {
            if ((mask & (1ul << i)) && (p->sigpending & (1ul << i))) {
                sig = i;
                p->sigpending &= ~(1ul << i);
                break;
            }
        }
    }
    /* ====================================================== */

    /* No signal matched */
    THREAD_SIGMASK_SET(t, old_mask);

    if (t->sleep_reason == 1) {
        TRACE_THREAD("SIGWAIT: timeout");
        return 0;
    }

    TRACE_THREAD("SIGWAIT: spurious wakeup");
    return EINTR;
}

/*
 * Block signals for the current thread (add to mask)
 */
long _cdecl proc_thread_signal_sigblock(ulong mask)
{
    struct thread *t = CURTHREAD;
    
    if (!t) return EINVAL;
        
    /* Merge assignment to add signals to mask, excluding unmaskable ones */
    THREAD_SIGMASK_ADD(t, mask);
    if ( t->tid == 0) {
        PROC_SIGMASK_ADD(t, mask);
    }
    TRACE_THREAD("proc_thread_signal_sigblock: TID=%d, added mask=0x%lx, new_mask=0x%lx",
                 t->tid, mask, THREAD_SIGMASK(t));
    return 0;
}

/*
 * Set the signal mask for the current thread
 */
long _cdecl sys_p_thread_sigsetmask(ulong mask)
{
    return proc_thread_signal_sigmask(mask);
}

/*
 * Temporarily set signal mask and pause until a signal is received
 */
long _cdecl sys_p_thread_sigpause(ulong mask)
{
    struct thread *t = CURTHREAD;
    struct proc *p = curproc;
    ulong old_mask;
    int sig;
    
    if (!t || !p)
        return EINVAL;
        
    /* Save old mask */
    old_mask = THREAD_SIGMASK(t);
    
    /* Set new mask */
    THREAD_SIGMASK_SET(t, mask);
    
    /* Wait for any signal */
    sig = proc_thread_signal_sigwait(~0UL, -1);
    
    /* Restore old mask */
    t->t_sigmask = old_mask;
    
    return sig;
}

/*
 * Clean up thread signal handlers for a thread
 */
void cleanup_thread_signals(struct thread *t)
{
    if (!t || !t->proc || !t->proc->p_sigacts || t->magic != CTXT_MAGIC) return;

    unsigned short i;

    /* Clean up any signal handlers registered by this thread */
    for (i = 0; i < NSIG; i++) {
        t->sig_handlers[i].handler = NULL;
        t->sig_handlers[i].arg = NULL;
    }

    /* Cancel any pending alarm for this thread */
    if (t->alarm_timeout) {
        canceltimeout(t->alarm_timeout);
        t->alarm_timeout = NULL;
    }
    
    /* Clear signal-related state */
    t->t_sig_in_progress = 0;
    t->t_sigpending = 0;
    TRACE_THREAD("CLEANUP THREAD SIGNALS: clearing sig_wait_obj for thread %d", t->tid);
    t->sig_wait_obj = NULL;
    
    /* Free signal stack if allocated */
    if (t->sig_stack) {
        kfree(t->sig_stack);
        t->sig_stack = NULL;
    }

    TRACE_THREAD("CLEANUP THREAD SIGNALS: thread %d cleaned up", t->tid);
    /* Clean up thread signal queue */
    cleanup_thread_sigqueue(t);
    TRACE_THREAD("CLEANUP THREAD SIGNALS: thread %d signal queue cleaned up", t->tid);
        
    return;
}

/*
 * Set an alarm for the current thread
 * Returns the number of milliseconds remaining in the previous alarm, or 0 if none
 * 
 * This function allows each thread to have its own independent alarm timer.
 * When the alarm expires, the thread receives a SIGALRM signal.
 * If a previous alarm was set, it returns the remaining time in milliseconds.
 */
long _cdecl proc_thread_signal_sigalrm(struct thread *t, long ms)
{
    if (!t) return EINVAL;

    long remaining = 0;
    struct proc *p = t->proc;

    /* Calculate remaining time on current alarm */
    if (t->alarm_timeout) {
        remaining = timeout_remaining(t->alarm_timeout);
        canceltimeout(t->alarm_timeout);
        t->alarm_timeout = NULL;
    }
    
    /* Set new alarm if requested */
    if (ms > 0) {
        t->alarm_timeout = addtimeout(p, ms, thread_signal_alarm_handler);
        if (t->alarm_timeout) {
            t->alarm_timeout->arg = (long)t;
        }
    }
    return remaining;
}

/*
 * Dispatch thread signals
 * Called during context switches and when threads wake up
 * Handles ALL pending unmasked signals, not just one
 */
void dispatch_thread_signals(struct thread *t)
{
    unsigned long sig;
    unsigned short signals_handled = 0;

    if (!t || !t->proc || !t->proc->p_sigacts || !t->proc->p_sigacts->thread_signals || t->is_idle){
        TRACE_THREAD("DISPATCH THREAD SIGNALS: ERROR - Invalid thread %d", t->tid);
        return;
    }

    /* Check if already handling a signal */
    if (t->t_sig_in_progress) {
        TRACE_THREAD("DISPATCH THREAD SIGNALS: WARNING - Thread %d already handling signal %d", t->tid, t->t_sig_in_progress);
        return;
    }

    /* Skip if blocked in sigwait - CRITICAL: But clear the flag! */
    if (t->wait_type & WAIT_SIGNAL) {
        TRACE_THREAD("DISPATCH THREAD SIGNALS: INFO - Thread %d is in sigwait, clearing WAIT_SIGNAL flag", t->tid);
        t->wait_type &= ~WAIT_SIGNAL; // Allow signal to be handled - DON'T return - let the signal be processed
    }

    TRACE_THREAD("DISPATCH THREAD SIGNALS: INFO - Checking thread %d for pending signals (0x%lx, mask=0x%lx)", 
                 t->tid, t->t_sigpending, THREAD_SIGMASK(t));

    /* Handle ALL pending signals with safety limit
     * Loop until no more signals or max iterations reached
     */
    while ((sig = check_thread_signals(t)) > 0 && signals_handled < NSIG) {
        /* Check if signal handler was set up */
        if (t->t_sig_in_progress) {
            TRACE_THREAD("DISPATCH THREAD SIGNALS: WARNING - Signal %d detected but signal %d already in progress", 
                         sig, t->t_sig_in_progress);
            break;
        }
        
        TRACE_THREAD("DISPATCH THREAD SIGNALS: INFO - Dispatching signal %ld to thread %d / Calling handle_thread_signal()", 
                     sig, t->tid);
        handle_thread_signal(t, sig);
        signals_handled++;
    }
    
    if (signals_handled >= NSIG) {
        TRACE_THREAD("DISPATCH THREAD SIGNALS: ERROR - dispatch_thread_signals hit iteration limit for thread %d", t->tid);
    }
    
    if (signals_handled > 0) {
        TRACE_THREAD("DISPATCH THREAD SIGNALS: INFO - Handled %d signal(s) for thread %d", signals_handled, t->tid);
    }
    TRACE_THREAD("DISPATCH THREAD SIGNALS: INFO - Finished checking thread %d for pending signals", t->tid);
    return;
}

/* Clean up signal stack if needed */
void cleanup_signal_stack(PROC *p, long arg)
{
    struct thread *t = (struct thread *)arg;
    
    if (!t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("CLEANUP SIGNAL STACK: Invalid thread pointer");
        return;
    }
        
    if (t->t_sig_in_progress == 0 && t->sig_stack) {
        void *stack_to_free = t->sig_stack;
        t->sig_stack = NULL;
        kfree(stack_to_free);
        TRACE_THREAD("CLEANUP SIGNAL STACK: Freed signal stack for thread %d", t->tid);
    }
    return;
}

/*
 * Broadcast a signal to all threads in a process (except thread0)
 * Useful for implementing process-wide notifications
 */
long _cdecl proc_thread_signal_broadcast(int sig)
{
    struct proc *p = curproc;
    if (!p || !p->p_sigacts || !p->p_sigacts->thread_signals)
        return EINVAL;

    /* Validate that this is a thread-specific signal */
    if (!IS_THREAD_SIGNAL(sig)) {
        TRACE_THREAD("THREAD SIGNAL BROADCAST: signal %d is not a valid thread signal", sig);
        return EINVAL;
    }

    /* Use the enhanced raise function which handles broadcasting */
    return proc_thread_signal_aware_raise(p, sig);
}

long _cdecl proc_thread_sigreturn(void)
{
    struct thread *t = CURTHREAD;
    struct proc *p = t->proc;

    CONTEXT *ctx = NULL;
    
    if (!t || !p) {
        TRACE_THREAD("SIGRETURN: Invalid thread or process");
        return EINVAL;
    }

    if (!t->t_sig_in_progress) {
        TRACE_THREAD("SIGRETURN: No signal in progress for thread %d", t->tid);
        return EINVAL;
    }
 
    TRACE_THREAD("SIGRETURN: Thread %d returning from signal %d handler", 
                 t->tid, t->t_sig_in_progress);
    
    /* Restore signal mask AFTER clearing signal state */
    THREAD_SIGMASK_SET(t, t->old_sigmask);

    ctx = &t->ctxt[SYSCALL];

    /* Restore the original context from before the signal */
    TRACE_THREAD("SIGRETURN: Restoring original context - PC=%lx, SSP=%lx, USP=%lx, SR=%x", 
                 t->saved_ctx.pc, t->saved_ctx.ssp, t->saved_ctx.usp, t->saved_ctx.sr);
    
    /* No more signals, return to restored context */
    memcpy(ctx, &t->saved_ctx, sizeof(CONTEXT));
    
    /* Sync to process context */
    TRACE_THREAD("SYNC SYS: Mirroring syscall context for thread %d", t->tid);
    memcpy(&t->proc->ctxt[SYSCALL], ctx, sizeof(CONTEXT));

    /* Clear the pending flag for the signal we just handled */
    unsigned long sig = t->t_sig_in_progress;

    /* CRITICAL: If sig is 0, this sigreturn was called from the classic sendsig() path,
    * not from thread_signal_trampoline(). In this case, the signal was already cleared
    * by check_sigs() in signal.c, so we don't need to clear it again.
    * Only clear thread-specific pending signals if t_sig_in_progress was set.
    */
    if (sig > 0 && sig < NSIG) {
        t->t_sigpending &= ~(1UL << sig);
        TRACE_THREAD("SIGRETURN: Cleared pending flag for signal %d, new pending 0x%lx", 
                    sig, t->t_sigpending);
    } else {
        TRACE_THREAD("SIGRETURN: No t_sig_in_progress (sig=%lu), signal already cleared by check_sigs()", sig);
    }
                
    /* Clear signal in progress flag */
    t->t_sig_in_progress = 0;

    /* Clean up signal stack */
    if (t->sig_stack) {
        kfree(t->sig_stack);
        t->sig_stack = NULL;
    }

    /* Check for more pending signals and dispatch them before rescheduling.
    * This ensures ALL pending signals are handled before returning to 
    * blocked state (e.g., pthread_join, sleep).
    */
    if (t->tid > 0 && t->t_sigpending && !(t->t_sigpending & THREAD_SIGMASK(t))) {
        sig = check_thread_signals(t);
        if (sig > 0) {
            TRACE_THREAD("SIGRETURN: Thread %d has more pending signals, handling before reschedule", t->tid);
            
            /* Dispatch the next signal immediately */
            handle_thread_signal(t, sig);
            
            /* This will recursively call sigreturn when that handler completes,
            * ensuring all signals are processed before we reach proc_thread_schedule()
            */
            return 0;
        }
    }

    proc_thread_schedule();
    
    /* Should never reach here */
    TRACE_THREAD("SIGRETURN ERROR: Returned from change_context!");
    return 0;
}

/* ============================================================================
 * Thread Signal Queue Management
 * ============================================================================ */

/**
 * Enqueue a signal with siginfo to a thread's signal queue
 */
static int enqueue_thread_signal_info(struct thread *t, int sig, const siginfo_t *info)
{
    struct sigqueue_entry *entry;
    unsigned short sr;
    
    if (!t || !info || sig <= 0 || sig >= NSIG) {
        return EINVAL;
    }
    
    /* Check queue limit per thread */
    if (t->t_sigqueue_count >= SIGQUEUE_MAX) {
        TRACE_THREAD("enqueue_thread_signal: queue full for thread %d (count=%d)", 
                     t->tid, t->t_sigqueue_count);
        return EAGAIN;
    }
    
    /* Allocate queue entry */
    entry = kmalloc(sizeof(*entry));
    if (!entry) {
        TRACE_THREAD("enqueue_thread_signal: out of memory");
        return ENOMEM;
    }
    
    /* Fill siginfo */
    memcpy(&entry->info, info, sizeof(siginfo_t));
    entry->queued = 1;
    entry->next = NULL;
    
    /* Add to thread's queue atomically */
    sr = splhigh();
    
    if (t->t_sigqueue_tail) {
        t->t_sigqueue_tail->next = entry;
    } else {
        t->t_sigqueue_head = entry;
    }
    t->t_sigqueue_tail = entry;
    t->t_sigqueue_count++;
    
    /* Also set pending bit */
    SET_THREAD_SIGPENDING(t, sig);
    
    spl(sr);
    
    TRACE_THREAD("enqueue_thread_signal: queued signal %d to thread %d (count=%d, value=%d)", 
                 sig, t->tid, t->t_sigqueue_count, info->si_value.sival_int);
    
    return 0;
}

/**
 * Dequeue a signal with siginfo from a thread's signal queue
 */
static int dequeue_thread_signal_info(struct thread *t, const sigset_t *set, siginfo_t *info)
{
    int sig = -1;
    struct sigqueue_entry *entry, *prev = NULL;
    unsigned short sr;
    
    if (!t || !set) {
        return -1;
    }
    
    sr = splhigh();
    
    /* Check thread's queued signals first (these have extended siginfo) */
    for (entry = t->t_sigqueue_head; entry; prev = entry, entry = entry->next) {
        if ((*set) & (1L << entry->info.si_signo)) {
            /* Found queued signal in set */
            if (info) {
                memcpy(info, &entry->info, sizeof(siginfo_t));
            }
            sig = entry->info.si_signo;
            
            /* Remove from queue */
            if (prev) {
                prev->next = entry->next;
            } else {
                t->t_sigqueue_head = entry->next;
            }
            
            if (entry == t->t_sigqueue_tail) {
                t->t_sigqueue_tail = prev;
            }
            
            t->t_sigqueue_count--;
            kfree(entry);
            
            /* Check if more instances of this signal are queued */
            int more_pending = 0;
            for (entry = t->t_sigqueue_head; entry; entry = entry->next) {
                if (entry->info.si_signo == sig) {
                    more_pending = 1;
                    break;
                }
            }
            
            /* Clear pending bit if no more queued instances */
            if (!more_pending && !(t->t_sigpending & (1L << sig))) {
                CLEAR_THREAD_SIGPENDING(t, sig);
            }
            
            spl(sr);
            
            TRACE_THREAD("dequeue_thread_signal: dequeued signal %d from thread %d (count=%d)", 
                         sig, t->tid, t->t_sigqueue_count);
            return sig;
        }
    }
    
    /* No queued signal found - check pending bitmask */
    ulong pending = (t->t_sigpending & (*set)) & ~1UL;
    if (pending) {
        int i;
        for (i = 1; i < NSIG; i++) {
            if (pending & (1L << i)) {
                sig = i;
                CLEAR_THREAD_SIGPENDING(t, sig);
                
                if (info) {
                    memset(info, 0, sizeof(siginfo_t));
                    info->si_signo = sig;
                    info->si_code = SI_USER;
                    info->si_pid = 0;
                    info->si_uid = 0;
                }
                
                spl(sr);
                return sig;
            }
        }
    }
    
    spl(sr);
    return -1;
}

/* Clean up thread signal queue */
static void cleanup_thread_sigqueue(struct thread *t)
{
    struct sigqueue_entry *entry, *next;
    unsigned short sr;
    
    if (!t) return;
    
    sr = splhigh();
    
    entry = t->t_sigqueue_head;
    while (entry) {
        next = entry->next;
        kfree(entry);
        entry = next;
    }
    
    t->t_sigqueue_head = NULL;
    t->t_sigqueue_tail = NULL;
    t->t_sigqueue_count = 0;
    
    spl(sr);
}

// void proc_thread_handle_proc_signal(void){

//     struct proc *p = get_curproc();
//     struct sigaction *sa;
//     if (p->p_flag & P_FLAG_SIGWAIT){ 
//         TRACE_THREAD("SCHED: Process %d is waiting for signal", p->pid);
//         return;
//     }
//     TRACE_THREAD("SCHED: Checking for pending signals for process %d", p->pid);
//     /* Check for fatal signals at process level before scheduling */
//     if (p->sigpending) {
//         int i;
//         for (i = 1; i < NSIG; i++) {
//             if (p->sigpending & (1UL << i)) {
//                 sa = &SIGACTION(p, i);
                
//                 /* Only scheduler can terminate the process */
//                 if (sa->sa_handler == SIG_DFL) {
//                     /* Check if this is a fatal signal */
//                     switch (i) {
//                         case SIGKILL:
//                         case SIGTERM:
//                         case SIGABRT:
//                         case SIGSEGV:
//                         case SIGBUS:
//                         case SIGILL:
//                         case SIGFPE:
//                         case SIGTRAP:
//                             TRACE_THREAD("SCHED: Fatal signal %d with SIG_DFL, terminating process %d", 
//                                          i, p->pid);
//                             p->sigpending &= ~(1UL << i);
//                             /* Call your existing process termination */
//                             handle_sig(i);
//                             /* Does not return */
//                             break;
//                     }
//                 }
//             }
//         }
//         TRACE_THREAD("SCHED: No fatal signals pending for process %d", p->pid);
//     }

// }