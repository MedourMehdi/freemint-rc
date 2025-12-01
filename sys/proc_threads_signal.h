/**
 * @file proc_threads_signal.h
 * @brief Thread Signal Handling Interface
 * 
 * Declares signal management APIs for threaded environments.
 * Defines per-thread signal masks, handler registration, and targeted
 * signal delivery mechanisms with POSIX-compliant semantics.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"

#ifndef PROC_THREADS_SIGNAL_H
#define PROC_THREADS_SIGNAL_H

/* signal flags */
# define SAS_OLDMASK	0x01		/* need to restore mask before pause */
# define SAS_THREADED	0x02		/* process uses thread-specific signals */

/* Thread signal handling macros */
# define IS_THREAD_SIGNAL(sig) ((sig) > 0 && (sig) < NSIG)
# define IS_THREAD_USER_SIGNAL(sig) ((sig) == SIGUSR1 || (sig) == SIGUSR2)
# define CLEAR_THREAD_SIGPENDING(t,s) ((t)->t_sigpending &= ~(1UL << (s))) 
# define THREAD_SIGNAL_MASK ((1UL << SIGUSR1) | (1UL << SIGUSR2)) /* Mask for all valid thread signals (only SIGUSR1 and SIGUSR2) */
# define SET_THREAD_SIGPENDING(t,s) ((t)->t_sigpending |= (1UL << (s)))

#define THREAD_SIGMASK(t)          ((t)->t_sigmask)
#define THREAD_SIGPENDING(t)       ((t)->t_sigpending)
/* Thread signal mask manipulation macros */
#define SET_THREAD_SIGMASK(t, mask) ((t)->t_sigmask = (mask))
#define GET_THREAD_SIGMASK(t) ((t)->t_sigmask)

/* Set signal mask for thread, excluding unmaskable signals */
#define THREAD_SIGMASK_SET(t, mask) do { \
    (t)->t_sigmask = (mask) & ~UNMASKABLE; \
} while(0)
 
/* Alternative version with validation */
#define THREAD_SIGMASK_SET_SAFE(t, mask) do { \
    if ((t) && (t)->magic == CTXT_MAGIC) { \
        (t)->t_sigmask = (mask) & ~UNMASKABLE; \
    } \
} while(0)

/* Add signals to thread mask (block additional signals) */
#define THREAD_SIGMASK_ADD(t, mask) do { \
    (t)->t_sigmask |= ((mask) & ~UNMASKABLE); \
} while(0)

/* Alternative version with validation */
#define THREAD_SIGMASK_ADD_SAFE(t, mask) do { \
    if ((t) && (t)->magic == CTXT_MAGIC) { \
        (t)->t_sigmask |= ((mask) & ~UNMASKABLE); \
    } \
} while(0)

/* Add a single signal to mask */
#define THREAD_SIGMASK_ADD_SIGNAL(t, sig) do { \
    if ((sig) > 0 && (sig) < NSIG && !((1UL << (sig)) & UNMASKABLE)) { \
        (t)->t_sigmask |= (1UL << (sig)); \
    } \
} while(0)

/* Function prototypes */
/* Enables or disables thread-specific signal handling for a process */
long _cdecl proc_thread_signal_mode(int enable);
/* Sends a signal to a specific thread within the current process */
long _cdecl proc_thread_signal_kill(struct thread *t, int sig);
/* Sets the signal mask for the current thread, returns previous mask */
long _cdecl proc_thread_signal_sigmask(ulong mask);
/* Block signals for the current thread (add to mask) */
long _cdecl proc_thread_signal_sigblock(ulong mask);
/* Waits for signals with timeout, returns signal number or 0 on timeout */
long _cdecl proc_thread_signal_sigwait(ulong mask, long timeout);

long _cdecl proc_thread_signal_broadcast(int sig);
/* Registers a thread-specific signal handler function */
long _cdecl proc_thread_signal_sighandler(int sig, void (*handler)(int, void*), void *arg);
/* Registers a thread-specific signal handler function with argument */
long _cdecl proc_thread_signal_sighandler_arg(int sig, void *arg);
/* Sets the alarm for the current thread */
long _cdecl proc_thread_signal_sigalrm(struct thread *t, long ms);

/* System call interfaces */
/* Sets the signal mask for the current thread, returns previous mask */
long _cdecl sys_p_thread_sigsetmask(ulong mask);
/* Temporarily set signal mask and pause until a signal is received */
long _cdecl sys_p_thread_sigpause(ulong mask);
/* Signal return from signal handler */
long _cdecl proc_thread_sigreturn(void);

/* Internal functions - to be called from thread lifecycle functions */
void cleanup_thread_signals(struct thread *t);
void cleanup_signal_stack(PROC *p, long arg);
/* Checks for pending signals in a thread, returns signal number or 0 */
int check_thread_signals(struct thread *t);
void handle_thread_signal(struct thread *t, int sig);

int dequeue_signal_info(PROC *p, struct thread *t, const sigset_t *set, siginfo_t *info);

/* Delivers a signal to a specific thread, returns 1 if delivered, 0 otherwise */
int deliver_signal_to_thread(struct proc *p, struct thread *t, int sig, const siginfo_t *info);

#endif /* PROC_THREADS_SIGNAL_H */