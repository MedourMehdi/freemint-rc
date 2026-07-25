/**
 * @file proc_threads_syscall.c
 * @brief Kernel Thread System Call Dispatcher
 * 
 * Routes pthread-related system calls to appropriate kernel subsystems.
 * Handles synchronization, signaling, scheduling, and cleanup operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"
#include "proc_threads_syscall.h"

#include "proc_threads_policy.h"
#include "proc_threads_signal.h"
#include "proc_threads_sync.h"
#include "proc_threads_sem.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_signal.h"
#include "proc_threads_sleep_yield.h"
#include "proc_threads_helper.h"
#include "proc_threads_tsd.h"
#include "proc_threads_cleanup.h"
#include "proc_threads_atomic.h"
#include "proc_threads_queue.h"

#include "dossig.h"

#ifndef __SIZE_T
#define __SIZE_T
typedef unsigned long size_t;
#endif

/* Memory access helper for single-address-space systems */
#ifndef copyout
#define copyout(src, dst, len) \
    (memcpy((void*)(dst), (const void*)(src), (size_t)(len)), 0)
#endif

#ifndef copyin
#define copyin(src, dst, len) \
    (memcpy((void*)(dst), (const void*)(src), (size_t)(len)), 0)
#endif

/* Parameter structures for RT signal calls */
struct sigtimedwait_params {
	const sigset_t *set;
	siginfo_t *info;
	const struct timespec *timeout;
};

struct sigqueue_params {
	long pid;
	long sig;
	union sigval value;
};

long _cdecl sys_p_thread_ctrl(long func, long arg1, long arg2) {

    TRACE_THREAD_VERBOSE("CTRL: sys_p_thread_ctrl called with func=%ld arg1=%ld arg2=%ld", func, arg1, arg2);

    switch (func) {

        case THREAD_CTRL_EXIT:
            TRACE_THREAD("SYSCALL THREAD_CTRL_EXIT");
            proc_thread_exit((void*)arg1, NULL);  /* Use arg1 as the return value */
            return 0;

        case THREAD_CTRL_SETCANCELSTATE: {

            struct thread *t = CURTHREAD;
            int new_state = (int)arg1;

            if (!t) return EINVAL;
            
            if (new_state != PTHREAD_CANCEL_ENABLE && 
                new_state != PTHREAD_CANCEL_DISABLE) {
                TRACE_THREAD("SETCANCELSTATE: Invalid cancel state %d", new_state);
                return EINVAL;
            }
            
            register unsigned short sr = splhigh();
            int old_state = t->cancel_state;
            t->cancel_state = new_state;
            
            TRACE_THREAD("SETCANCELSTATE: Thread %d changed cancel state from %d to %d", t->tid, old_state, new_state);

            // If oldstate pointer provided, store previous state
            if (arg2) {
                if (copyout(&old_state, (void*)arg2, sizeof(int))) {
                    spl(sr);
                    TRACE_THREAD("SETCANCELSTATE: copyout failed for old_state");
                    return EFAULT;
                }
            }
            
            spl(sr);
            return 0;
        }
        
        case THREAD_CTRL_SETCANCELTYPE: {
            struct thread *t = CURTHREAD;
            if (!t) return EINVAL;
            
            int new_type = (int)arg1;
            if (new_type != PTHREAD_CANCEL_DEFERRED && 
                new_type != PTHREAD_CANCEL_ASYNCHRONOUS) {
                TRACE_THREAD("SETCANCELTYPE: Invalid cancel type %d", new_type);
                return EINVAL;
            }
            
            register unsigned short sr = splhigh();
            int old_type = t->cancel_type;
            t->cancel_type = new_type;
            
            TRACE_THREAD("SETCANCELTYPE: Thread %d changed cancel type from %d to %d", t->tid, old_type, new_type);

            if (arg2) {
                if (copyout(&old_type, (void*)arg2, sizeof(int))) {
                    spl(sr);
                    TRACE_THREAD("SETCANCELTYPE: copyout failed for old_type");
                    return EFAULT;
                }
            }
            TRACE_THREAD("SETCANCELTYPE: Thread %d cancel type set to %d", t->tid, new_type);
            spl(sr);
            return 0;
        }
        
        case THREAD_CTRL_TESTCANCEL: {
            struct thread *t = CURTHREAD;
            if (!t) return EINVAL;
            
            register unsigned short sr = splhigh();
            int should_cancel = (t->cancel_state == PTHREAD_CANCEL_ENABLE && 
                                t->cancel_pending);
            spl(sr);
            
            TRACE_THREAD("TESTCANCEL: Thread %d should_cancel=%d", t->tid, should_cancel);

            if (should_cancel) {
                TRACE_THREAD("TESTCANCEL: Thread %d cancelling itself", t->tid);
                // Exit current thread (t = NULL means current thread exits itself)
                proc_thread_exit(PTHREAD_CANCELED, NULL);
            }
            return 0;
        }
        
        case THREAD_CTRL_CANCEL: {
            struct thread *target = proc_thread_find(curproc, (short)arg2);
            if (!target) return ESRCH;
            
            register unsigned short sr = splhigh();

            TRACE_THREAD("CANCELLING THREAD %d", target->tid);

            target->cancel_pending = 1;
            
            // For ASYNCHRONOUS mode, send a signal to interrupt the target thread
            if (target->cancel_state == PTHREAD_CANCEL_ENABLE && 
                target->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) {
                
                // If target is sleeping/waiting, wake it up
                if (target->state == THREAD_STATE_BLOCKED) {
                    proc_thread_state_change(target, THREAD_STATE_READY);
                    add_to_ready_queue(target);
                }
                
                // Set a flag that the scheduler will check
                target->cancel_requested = 1;
            }
            
            spl(sr);
            return 0;
        }

        case THREAD_CTRL_STATUS:
            TRACE_THREAD("THREAD_CTRL_STATUS");
            // Get thread status
            return proc_thread_status(arg1);

        case THREAD_CTRL_GETID:
            TRACE_THREAD_VERBOSE("THREAD_CTRL_GETID");
            return sys_p_thread_getid();

        case THREAD_CTRL_GET_ERRNO_PTR:
            TRACE_THREAD_VERBOSE("THREAD_CTRL_GET_ERRNO_PTR");
            return (long)CURTHREAD->errno_ptr;  // May be NULL

        case THREAD_CTRL_SET_ERRNO_PTR:
            TRACE_THREAD("THREAD_CTRL_SET_ERRNO_PTR: Setting errno_ptr to %p", (void *)arg1);
            CURTHREAD->errno_ptr = (int *)arg1;
            return 0;

        case THREAD_CTRL_SETNAME: {
            short tid = (short)arg1;
            char *user_name = (char *)arg2;
            struct thread *target = proc_thread_find(curproc, tid);
            char kname[16];

            TRACE_THREAD("THREAD_CTRL_SETNAME: Setting name for thread %d to '%s'", tid, user_name);

            if (!target) {
                TRACE_THREAD("THREAD_CTRL_SETNAME: No such thread %d", tid);
                return ESRCH;
            }

            
            if (copyin(user_name, kname, 16)) {
                TRACE_THREAD("THREAD_CTRL_SETNAME: Copyin failed");
                return EFAULT;
            }
            kname[15] = '\0'; // Ensure null termination
            
            strcpy(target->name, kname);
            TRACE_THREAD("THREAD_CTRL_SETNAME: Thread %d name set to '%s'", tid, target->name);
            return 0;
        }
        
        case THREAD_CTRL_GETNAME: {
            short tid = (short)arg1;
            char *user_buf = (char *)arg2;
            struct thread *target = proc_thread_find(curproc, tid);
            TRACE_THREAD("THREAD_CTRL_GETNAME: Getting name for thread %d", tid);
            if (!target) return ESRCH;
            
            if (copyout(target->name, user_buf, 16)) return EFAULT;
            return 0;
        }

        case THREAD_CTRL_IS_INITIAL: {
            struct thread *t = CURTHREAD;
            TRACE_THREAD("THREAD_CTRL_IS_INITIAL: Thread %d is initial", t ? t->tid : -1);
            if (!t) return 0; // Not a thread? Then not initial
            return (t->tid == 0) ? 1 : 0;
        }

        case THREAD_CTRL_IS_MULTITHREADED: {
            struct proc *p = curproc;
            TRACE_THREAD("THREAD_CTRL_IS_MULTITHREADED: Process %d has %d threads", p ? p->pid : -1, p ? p->num_threads : -1);
            if (!p || !p->threads) return 0;
            return (p->num_threads > 1) ? 1 : 0;
        }
        
        case THREAD_CTRL_SWITCH_TO_MAIN: {
            struct proc *p = curproc;
            if (!p) {
                return EINVAL;
            }

            struct thread *main_thread = get_main_thread(p);
            if (!main_thread || main_thread->magic != CTXT_MAGIC) {
                return ESRCH;
            }

            struct thread *current = p->current_thread;
            if (!current) {
                return EINVAL;
            }

            // Already on main thread
            if (current->tid == 0) {
                return 0;
            }

            // Check if main thread is runnable
            if (main_thread->state != THREAD_STATE_READY) {
                return EAGAIN;
            }

            register unsigned short sr = splhigh();


            TRACE_THREAD("SWITCH_TO_MAIN: Switching from thread %d to main thread %d", current->tid, main_thread->tid);
            // Prepare current thread for rescheduling
            proc_thread_state_change(current, THREAD_STATE_READY);
            add_to_ready_queue(current);

            // Prepare main thread for execution
            proc_thread_state_change(main_thread, THREAD_STATE_RUNNING);
            p->current_thread = main_thread;

            // Remove main thread from queues if present
            if (is_in_ready_queue(main_thread)) {
                remove_from_ready_queue(main_thread);
            }
            remove_thread_from_wait_queues(main_thread);

            // Perform context switch
            thread_switch(current, main_thread);

            spl(sr);
            return 0;
        }

        case THREAD_CTRL_SWITCH_TO_THREAD: {
            short target_tid = (short)arg1;
            struct proc *p = curproc;
            struct thread *current = p->current_thread;
            struct thread *target = NULL;

            // Find target thread
            struct thread *t = NULL;

            TRACE_THREAD("SWITCH_TO_THREAD: Attempting to switch from thread %d to thread %d", current ? current->tid : -1, target_tid);

            for (t = p->threads; t != NULL; t = t->next) {
                if (t->tid == target_tid && t->magic == CTXT_MAGIC && 
                    !(t->state & THREAD_STATE_EXITED)) {
                    target = t;
                    break;
                }
            }

            if (!target) {
                TRACE_THREAD("SWITCH_TO_THREAD: Thread %d not found", target_tid);
                return ESRCH;
            }

            // Validate thread state
            if (target->state != THREAD_STATE_READY || target == current) {
                TRACE_THREAD("SWITCH_TO_THREAD: Thread %d not ready or is current", target_tid);
                return EAGAIN;
            }

            // Remove from ready queue
            remove_from_ready_queue(target);

            // Update states
            if (current->wait_type == WAIT_NONE) {
                proc_thread_state_change(current, THREAD_STATE_READY);
                add_to_ready_queue(current);
            }
            
            proc_thread_state_change(target, THREAD_STATE_RUNNING);
            // p->current_thread = target;  --- IGNORE --- Handled by thread_switch()
            // target->last_scheduled = get_system_ticks(); --- IGNORE --- Handled in thread_switch()

            // Perform context switch
            TRACE_THREAD("SWITCH_TO_THREAD: Switching %d -> %d", current->tid, target->tid);
            thread_switch(current, target);
            
            return 0;
        }
        case THREAD_CTRL_SIGRETURN:
            TRACE_THREAD("THREAD_CTRL_SIGRETURN");
            return proc_thread_sigreturn();
        case THREAD_CTRL_SETUP_THREADING:
            TRACE_THREAD("THREAD_CTRL_SETUP_THREADING");
            if(handle_thread_mode_switching(curproc)) {
                return 0;
            }
            return EAGAIN;
        default:
            TRACE_THREAD("ERROR: sys_p_thread_ctrl called with invalid func %d", func);
            return EINVAL;
    }
}

long _cdecl sys_p_thread_signal(long func, long arg1, long arg2) {

    TRACE_THREAD_VERBOSE("SYSCALL sys_p_thread_signal: func=%ld, arg1=%ld, arg2=%ld", func, arg1, arg2);

    switch (func) {
        case PTSIG_MODE:
            TRACE_THREAD("proc_thread_signal_mode: %s thread signals",  arg1 ? "enabling" : "disabling");
            return proc_thread_signal_mode((int)arg1);

        case PTSIG_KILL:
            {        
            TRACE_THREAD("PTSIG_KILL: arg1=%ld, arg2=%ld", arg1, arg2);
            long retval;
            /* Send signal to specific thread */
            struct thread *target = NULL;
            struct proc *p = curproc;
            
            if(!p->current_thread || !p->total_threads) {
                TRACE_THREAD("PTSIG_KILL: No threads in process %d", p->pid);

                return ESRCH;
            }

            TRACE_THREAD("PTSIG_KILL: Process %d has %d threads", p->pid, p->total_threads);
            /* Find thread by ID */
            struct thread *t;
            for (t = p->threads; t != NULL; t = t->next) {
                if (t->tid == arg1) {
                    target = t;
                    break;
                }
            }
            
            if (!target) {
                TRACE_THREAD("PTSIG_KILL: Thread with ID %ld not found", arg1);
                return ESRCH;
            }
            
            /* Send signal to target thread */
            TRACE_THREAD("PTSIG_KILL: Trying to Send signal %ld to thread %d", arg2, target->tid);
            retval = proc_thread_signal_kill(target, (int)arg2);
            TRACE_THREAD("PTSIG_KILL: Returning %ld to userspace", retval);
            return retval;
            }

        case PTSIG_GETMASK:
            if (CURTHREAD) {
                TRACE_THREAD("K: PTSIG_GETMASK returning t->t_sigmask=0x%08lx for tid=%d",
                            (ulong)CURTHREAD->t_sigmask, CURTHREAD->tid);
                /* If TID 0, we must ensure we return the authoritative process mask */
                if (CURTHREAD->tid == 0) {
                    return CURTHREAD->proc->p_sigmask;
                }
                return CURTHREAD->t_sigmask;
            }
            return 0;
        case PTSIG_SETMASK:
            TRACE_THREAD("K: PTSIG_SETMASK arg=0x%08lx for tid=%d (before t_sigmask=0x%08lx)",
                    (ulong)arg1, CURTHREAD ? CURTHREAD->tid : -1, CURTHREAD ? CURTHREAD->t_sigmask : 0UL);
            return proc_thread_signal_sigmask((ulong)arg1);
            
        case PTSIG_BLOCK:
            {   
                #ifdef DEBUG_THREAD
                struct thread *t = CURTHREAD;
                TRACE_THREAD("K: PTSIG_BLOCK arg=0x%08lx for tid=%d (before t_sigmask=0x%08lx)",
                            (ulong)arg1, t ? t->tid : -1, t ? t->t_sigmask : 0UL);
                #endif
                return proc_thread_signal_sigblock((ulong)arg1);
            }
        case PTSIG_UNBLOCK:
            {
                struct thread *t = CURTHREAD;
                if (!t) return EINVAL;
                TRACE_THREAD("K: PTSIG_UNBLOCK arg=0x%08lx for tid=%d (before t_sigmask=0x%08lx)",
                            (ulong)arg1, t ? t->tid : -1, t ? t->t_sigmask : 0UL);                
                t->t_sigmask &= ~(arg1 & ~UNMASKABLE);
                if (t->tid == 0) {
                    t->proc->p_sigmask &= ~(arg1 & ~UNMASKABLE);
                }
                return 0;
            }
            
        case PTSIG_WAIT:
            return proc_thread_signal_sigwait((ulong)arg1, (long)arg2);
            
        case PTSIG_HANDLER:
            TRACE_THREAD("sys_p_thread_signal -> proc_thread_signal_sighandler: PROC ID %d, THREAD ID %d, SIG %ld, HANDLER %lx, ARG %p", 
                        curproc ? curproc->pid : -1, 
                        CURTHREAD ? CURTHREAD->tid : -1,
                        arg1, arg2, NULL);
            return proc_thread_signal_sighandler((int)arg1, (void (*)(int, void*))arg2, NULL);
            
        case PTSIG_HANDLER_ARG:
            TRACE_THREAD("sys_p_thread_signal -> proc_thread_signal_sighandler_arg: PROC ID %d, THREAD ID %d, SIG %ld, ARG %ld", 
                        curproc ? curproc->pid : -1, 
                        CURTHREAD ? CURTHREAD->tid : -1,
                        arg1, arg2);
            return proc_thread_signal_sighandler_arg((int)arg1, (void*)arg2);

        case PTSIG_PENDING:
           {
               struct thread *t = CURTHREAD;
               if (!t) return EINVAL;
               return t->t_sigpending;
           }            
        case PTSIG_ALARM:
            return proc_thread_signal_sigalrm(CURTHREAD, arg1);

        case PTSIG_BROADCAST:
            return proc_thread_signal_broadcast(arg1);

        case PTSIG_WAITINFO:
            /* sigwaitinfo(const sigset_t *set, siginfo_t *info) 
             * arg1 = set, arg2 = info
             */
            TRACE_THREAD("PTSIG_WAITINFO: set=%p, info=%p", (void*)arg1, (void*)arg2);
            return sys_p_sigwaitinfo((const sigset_t *)arg1, (siginfo_t *)arg2);
            
        case PTSIG_TIMEDWAIT:
            /* sigtimedwait - parameters passed via structure
             * arg1 = pointer to struct sigtimedwait_params
             */
            {
                struct sigtimedwait_params params;
                
                if (copyin((void*)arg1, &params, sizeof(params))) {
                    TRACE_THREAD("PTSIG_TIMEDWAIT: copyin failed");
                    return EFAULT;
                }
                
                TRACE_THREAD("PTSIG_TIMEDWAIT: set=%p, info=%p, timeout=%p",
                            params.set, params.info, params.timeout);
                
                return sys_p_sigtimedwait(params.set, params.info, params.timeout);
            }
            
        case PTSIG_QUEUE:
            /* sigqueue - parameters passed via structure
             * arg1 = pointer to struct sigqueue_params
             */
            {
                struct sigqueue_params params;
                #ifdef DEBUG_THREAD
                struct sigqueue_params *params_debug = (struct sigqueue_params *)arg1;
                TRACE_THREAD("PTSIG_QUEUE: arg1=%p (address of params struct)", (void*)arg1);
                TRACE_THREAD("PTSIG_QUEUE: before copyin - pid=%ld, sig=%ld, value=%ld/%p",
                            params_debug->pid, params_debug->sig, 
                            params_debug->value.sival_int, params_debug->value.sival_ptr);
                #endif
                if (copyin((void*)arg1, &params, sizeof(params))) {
                    TRACE_THREAD("PTSIG_QUEUE: copyin failed");
                    return EFAULT;
                }
                
                TRACE_THREAD("PTSIG_QUEUE: after copyin - pid=%ld, sig=%ld, value=%ld/%p",
                            params.pid, params.sig, 
                            params.value.sival_int, params.value.sival_ptr);
                
                return sys_p_sigqueue((short)params.pid, (int)params.sig, (const union sigval)params.value);
            }

        case PTSIG_EXT_HANDLER:
            TRACE_THREAD("PTSIG_EXT_HANDLER: sig=%ld, handler=%lx", arg1, arg2);
            return sys_p_sigaction_ext((short)arg1, (void (*)(int, siginfo_t *, void *))arg2);

        default:
            if (func > 0 && func < NSIG) {
                /* Direct signal to thread */
                TRACE_THREAD("Sending signal %ld to thread ID %ld", func, arg1);
                struct thread *target = NULL;
                struct proc *p = curproc;
                
                if (arg1 == 0) {
                    /* Signal current thread */
                    target = CURTHREAD;
                    TRACE_THREAD("Using current thread (ID %d)", target ? target->tid : -1);
                } else {
                    /* Find thread by ID */
                    register unsigned short sr = splhigh();
                    struct thread *t;
                    for (t = p->threads; t != NULL; t = t->next) {
                        if (t->tid == arg1) {
                            target = t;
                            TRACE_THREAD("Found thread with ID %ld", arg1);
                            break;
                        }
                    }
                    spl(sr);
                    
                    if (!target) {
                        TRACE_THREAD("Thread with ID %ld not found", arg1);
                        return ESRCH;
                    }
                }
                
                /* Now deliver the signal to the target thread */
                if (target) {
                    TRACE_THREAD("Sending signal %ld to thread ID %ld", func, target->tid);
                    return proc_thread_signal_kill(target, (int)func);
                }
            }
            
            TRACE_THREAD("Invalid function code: %ld", func);
            return EINVAL;
    }
}

long _cdecl sys_p_thread_sync(long operator, long arg1, long arg2) {

    switch (operator) {

        case THREAD_SYNC_SEM_TRYWAIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_SEM_TRYWAIT");
            return thread_semaphore_trydown((struct semaphore *)arg1);

        case THREAD_SYNC_SEM_TIMEDWAIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_SEM_TIMEDWAIT");
            return thread_semaphore_timeddown((struct semaphore *)arg1, arg2); 

        case THREAD_SYNC_SEM_WAIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_SEM_WAIT");
            return thread_semaphore_down((struct semaphore *)arg1);

        case THREAD_SYNC_SEM_POST:
            TRACE_THREAD("SYSCALL THREAD_SYNC_SEM_POST, arg1=%p", arg1);
            return thread_semaphore_up((struct semaphore *)arg1);

        case THREAD_SYNC_MUTEX_LOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEX_LOCK");
            return thread_mutex_lock((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_TRYLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEX_TRYLOCK");
            return thread_mutex_trylock((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_UNLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEX_UNLOCK");
            return thread_mutex_unlock((struct mutex *)arg1);
            
        case THREAD_SYNC_MUTEX_INIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEX_INIT, arg1=%p, arg2=%p", arg1, arg2);
            return thread_mutex_init((struct mutex *)arg1, (const struct mutex_attr *)arg2);

        case THREAD_SYNC_MUTEX_DESTROY:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEX_DESTROY");
            return thread_mutex_destroy((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_ATTR_INIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_INIT");
            return thread_mutexattr_init((struct mutex_attr *)arg1);

        case THREAD_SYNC_MUTEX_ATTR_DESTROY:
            TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_DESTROY");
            return thread_mutexattr_destroy((struct mutex_attr *)arg1);
            
        case THREAD_SYNC_MUTEXATTR_SETTYPE:
            {
                if (!CURTHREAD) handle_thread_mode_switching(curproc);

                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int type = (int)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_SETTYPE");
                if (!attr) return EINVAL;
                if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
                attr->type = type;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_SETPROTOCOL:
            {
                if (!CURTHREAD) handle_thread_mode_switching(curproc);

                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int protocol = (int)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_SETPROTOCOL");
                if (!attr) return EINVAL;
                if (protocol < PTHREAD_PRIO_NONE || protocol > PTHREAD_PRIO_PROTECT) return EINVAL;
                attr->protocol = protocol;
                return THREAD_SUCCESS;
            }
            
        case THREAD_SYNC_MUTEXATTR_SETPRIOCEILING:
            {
                if (!CURTHREAD) handle_thread_mode_switching(curproc);

                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int prioceiling = (int)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_SETPRIOCEILING");
                if (!attr) return EINVAL;
                if (prioceiling < 0 || prioceiling > MAX_POSIX_THREAD_PRIORITY) return EINVAL;
                attr->prioceiling = prioceiling;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETTYPE:
            {
                if (!CURTHREAD) handle_thread_mode_switching(curproc);

                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *type = (long *)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_GETTYPE");
                if (!user_attr || !type) return EINVAL;
                *type = user_attr->type;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETPRIOCEILING:
            {
                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *prioceiling = (long *)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_GETPRIOCEILING");
                if (!user_attr || !prioceiling) return EINVAL;
                *prioceiling = user_attr->prioceiling;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETPROTOCOL:
            {
                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *protocol = (long *)arg2;

                TRACE_THREAD("SYSCALL THREAD_SYNC_MUTEXATTR_GETPROTOCOL");
                if (!user_attr || !protocol) return EINVAL;
                *protocol = user_attr->protocol;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_SEM_INIT:
            /* 
             * Actually no mintlib calls exists for this
             * Semaphore is directly initialized in userspace
            */
            TRACE_THREAD("SYSCALL THREAD_SYNC_SEM_INIT, arg1=%p, arg2=%ld", arg1, arg2);
            return thread_semaphore_init((struct semaphore *)arg1, (long)arg2);

        case THREAD_SYNC_JOIN:
            TRACE_THREAD("SYSCALL THREAD_SYNC_JOIN");
            return proc_thread_join(arg1, (void**)arg2);
            
        case THREAD_SYNC_DETACH:
            TRACE_THREAD("SYSCALL THREAD_SYNC_DETACH");
            return proc_thread_detach(arg1);

        case THREAD_SYNC_SLEEP:
            TRACE_THREAD("SYSCALL THREAD_SYNC_SLEEP");
            return proc_thread_sleep((long)arg1);

        case THREAD_SYNC_YIELD:
            TRACE_THREAD("SYSCALL THREAD_SYNC_YIELD");
            return proc_thread_yield();

        case THREAD_SYNC_COND_INIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_INIT");
            return proc_thread_condvar_init((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_DESTROY:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_DESTROY");
            return proc_thread_condvar_destroy((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_WAIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_WAIT");
            return proc_thread_condvar_wait((struct condvar *)arg1, (struct mutex *)arg2);
            
        case THREAD_SYNC_COND_TIMEDWAIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_TIMEDWAIT");
            return proc_thread_condvar_timedwait((struct condvar *)arg1, (struct mutex *)arg2, ((struct condvar *)arg1)->timeout_ms);
            
        case THREAD_SYNC_COND_SIGNAL:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_SIGNAL");
            return proc_thread_condvar_signal((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_BROADCAST:
            TRACE_THREAD("SYSCALL THREAD_SYNC_COND_BROADCAST");
            return proc_thread_condvar_broadcast((struct condvar *)arg1);

        case THREAD_SYNC_CLEANUP_PUSH:
            TRACE_THREAD("SYSCALL THREAD_SYNC_CLEANUP_PUSH");
            return thread_cleanup_push((void (*)(void*))arg1, (void*)arg2);
            
        case THREAD_SYNC_CLEANUP_POP:
            TRACE_THREAD("SYSCALL THREAD_SYNC_CLEANUP_POP");
            return thread_cleanup_pop((void (**)(void*))arg1, (void**)arg2);

        case THREAD_SYNC_CLEANUP_GET:
            TRACE_THREAD("SYSCALL THREAD_SYNC_CLEANUP_GET");
            return get_cleanup_handlers(CURTHREAD, (struct cleanup_info*)arg1, (int)arg2);
        
        case THREAD_TSD_CREATE_KEY:
            TRACE_THREAD("SYSCALL THREAD_TSD_CREATE_KEY");
            return thread_key_create((void (*)(void*))arg1);
            
        case THREAD_TSD_DELETE_KEY:
            TRACE_THREAD("SYSCALL THREAD_TSD_DELETE_KEY");
            return thread_key_delete(arg1);
            
        case THREAD_TSD_GET_SPECIFIC:
            TRACE_THREAD("SYSCALL THREAD_TSD_GET_SPECIFIC");
            return (long)thread_getspecific(arg1);
            
        case THREAD_TSD_SET_SPECIFIC:
            TRACE_THREAD("SYSCALL THREAD_TSD_SET_SPECIFIC");
            return thread_setspecific(arg1, (void*)arg2);

        case THREAD_SYNC_RWLOCK_INIT:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_INIT");
            return thread_rwlock_init();

        case THREAD_SYNC_RWLOCK_DESTROY:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_DESTROY");
            return thread_rwlock_destroy(arg1);
            
        case THREAD_SYNC_RWLOCK_RDLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_RDLOCK");
            return thread_rwlock_rdlock(arg1);
            
        case THREAD_SYNC_RWLOCK_TRYRDLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_TRYRDLOCK");
            return thread_rwlock_tryrdlock(arg1);
            
        case THREAD_SYNC_RWLOCK_WRLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_WRLOCK");
            return thread_rwlock_wrlock(arg1);
            
        case THREAD_SYNC_RWLOCK_TRYWRLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_TRYWRLOCK");
            return thread_rwlock_trywrlock(arg1);
            
        case THREAD_SYNC_RWLOCK_UNLOCK:
            TRACE_THREAD("SYSCALL THREAD_SYNC_RWLOCK_UNLOCK");
            return thread_rwlock_unlock(arg1);
        
        default:
            TRACE_THREAD("SYSCALL THREAD_SYNC_UNKNOWN: %d", operator);
            return EINVAL;

    }

}

long _cdecl sys_p_pthread(long syscall_func, long arg1, long arg2, long arg3) {

    TRACE_THREAD_VERBOSE("SYSCALL - IN KERNEL: sys_p_pthread: CURPROC ID =%d, CURTHREAD ID =%d", curproc->pid, CURTHREAD ? CURTHREAD->tid : -1);
    
    switch (syscall_func) {

        case P_THREAD_CTRL:
            TRACE_THREAD_VERBOSE("SYSCALL P_THREAD_CTRL: arg1=%ld, arg2=%ld", arg1, arg2);
            return sys_p_thread_ctrl(arg1, arg2, arg3);
            
        case P_THREAD_SYNC:
            TRACE_THREAD_VERBOSE("SYSCALL P_THREAD_SYNC: arg1=%ld, arg2=%ld, arg3=%ld", arg1, arg2, arg3);
            return sys_p_thread_sync(arg1, arg2, arg3);
            
        case P_THREAD_SIGNAL:
            TRACE_THREAD_VERBOSE("SYSCALL P_THREAD_SIGNAL: arg1=%ld, arg2=%ld, arg3=%ld", arg1, arg2, arg3);
            return sys_p_thread_signal(arg1, arg2, arg3);
            
        case PSCHED_SETPARAM:
            TRACE_THREAD("SYSCALL PSCHED_SETPARAM");
            return proc_thread_set_schedparam(arg1, arg2, arg3);
            
        case PSCHED_GETPARAM:
            TRACE_THREAD("SYSCALL PSCHED_GETPARAM");
            return proc_thread_get_schedparam(arg1, (long*)arg2, (long*)arg3);
            
        case PSCHED_GETRRINTERVAL:
            TRACE_THREAD("SYSCALL PSCHED_GETRRINTERVAL");
            return proc_thread_get_rrinterval(arg1, (long*)arg2);
            
        case PSCHED_SET_TIMESLICE:
            TRACE_THREAD("SYSCALL PSCHED_SET_TIMESLICE");
            return proc_thread_set_timeslice(arg1, arg2);
            
        case PSCHED_GET_TIMESLICE:
            TRACE_THREAD("SYSCALL PSCHED_GET_TIMESLICE");
            return proc_thread_get_timeslice(arg1, (long*)arg2, (long*)arg3);

        case THREAD_ATOMIC_INCREMENT:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_INCREMENT");
            return !((volatile long *)arg1) ? EINVAL : atomic_increment((volatile long *)arg1);
            
        case THREAD_ATOMIC_DECREMENT:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_DECREMENT");
            return !((volatile long *)arg1) ? EINVAL : atomic_decrement((volatile long *)arg1);
            
        case THREAD_ATOMIC_CAS:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_CAS");
            return !((volatile long *)arg1) ? EINVAL : atomic_cas((volatile long *)arg1, arg2, arg3);
            
        case THREAD_ATOMIC_EXCHANGE:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_EXCHANGE");
            return !((volatile long *)arg1) ? EINVAL : atomic_exchange((volatile long *)arg1, (long)arg2);
            
        case THREAD_ATOMIC_ADD:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_ADD");
            return !((volatile long *)arg1) ? EINVAL : atomic_add((volatile long *)arg1, (long)arg2);
            
        case THREAD_ATOMIC_SUB:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_SUB");
            return !((volatile long *)arg1) ? EINVAL : atomic_sub((volatile long *)arg1, (long)arg2);
            
        case THREAD_ATOMIC_OR:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_OR");
            return !((volatile long *)arg1) ? EINVAL : atomic_or((volatile long *)arg1, (long)arg2);
            
        case THREAD_ATOMIC_AND:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_AND");
            return !((volatile long *)arg1) ? EINVAL : atomic_and((volatile long *)arg1, (long)arg2);
            
        case THREAD_ATOMIC_XOR:
            TRACE_THREAD("SYSCALL THREAD_ATOMIC_XOR");
            return !((volatile long *)arg1) ? EINVAL : atomic_xor((volatile long *)arg1, (long)arg2);

        case THREAD_ATOMIC_TAS:
            TRACE_THREAD_VERBOSE("SYSCALL THREAD_ATOMIC_TAS");
            return !((volatile unsigned short *)arg1) ? EINVAL : atomic_tas_try_lock((volatile unsigned short *)arg1);

        default:
            TRACE_THREAD("SYSCALL P_THREAD_UNKNOWN: %ld", syscall_func);
            return EINVAL;
    }

}