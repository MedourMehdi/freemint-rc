/*
 * FreeMiNT Kernel Threading Implementation
 * ----------------------------------------
 * 
 * POSIX-compliant threading subsystem for FreeMiNT kernel
 * 
 * Provides core threading infrastructure including:
 *  - Thread scheduling and context switching
 *  - Synchronization primitives (mutexes/condvars/semaphores)
 *  - Signal handling
 *  - Thread-specific data
 *  - Cleanup routines
 * 
 * Key components:
 *  proc_threads_sync.[ch]      - Synchronization objects
 *  proc_threads_mutex.[ch]     - Mutexes
 *  proc_threads_scheduler.[ch] - Scheduler core
 *  proc_threads_policy.[ch]    - Scheduling policies
 *  proc_threads_signal.[ch]    - Signal handling
 *  proc_threads_tsd.[ch]       - Thread-specific data
 *  proc_threads_cleanup.[ch]   - Cleanup handlers
 *  proc_threads_sleep_yield.[ch] - Sleep/yield mechanisms
 *  proc_threads_syscall.c      - System call interface
 *  proc_threads_queue.[ch]     - Queue management (ready/sleep/wait)
 *  proc_threads.[ch]           - Core thread management
 *  proc_threads_helper.[ch]    - Utility functions
 *  proc_threads_debug.[ch]     - Debugging facilities
 * 
 * Optimized for Motorola 68000-series processors
 * with tight memory constraints (Atari ST/STE/TT/Falcon)
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 * License: GPLv2
 */

#include "libkern/libkern.h" /* memset and memcpy */

#include "proc.h"
#include "mint/signal.h"
#include "timeout.h"
#include "arch/context.h"
#include "mint/arch/asm_spl.h"
#include "kmemory.h"
#include "kentry.h"
#include "proc_threads_debug.h"

#ifndef PROC_THREAD_H
#define PROC_THREAD_H

/* ============================================================================
 * THREAD STATES AND WAIT TYPES
 * ============================================================================ */

/* Thread States (bitfield values) */
#define THREAD_STATE_RUNNING    0x0001  /* Currently executing */
#define THREAD_STATE_READY      0x0002  /* On run queue, can be scheduled */
#define THREAD_STATE_BLOCKED    0x0004  /* Sleeping, waiting for event */
#define THREAD_STATE_STOPPED    0x0008  /* Stopped by signal */
#define THREAD_STATE_ZOMBIE     0x0010  /* Exited, not yet reaped */
#define THREAD_STATE_DEAD       0x0020  /* Fully dead, resources can be freed */

/* Composite states for convenience */
#define THREAD_STATE_EXITED     (THREAD_STATE_ZOMBIE | THREAD_STATE_DEAD)
#define THREAD_STATE_LIVE       (THREAD_STATE_RUNNING | THREAD_STATE_READY)

/* Wait Types (bitfield values) */
#define WAIT_NONE       0x0000  /* Not waiting */
#define WAIT_JOIN       0x0001  /* Waiting for thread to exit */
#define WAIT_MUTEX      0x0002  /* Waiting for mutex */
#define WAIT_CONDVAR    0x0004  /* Waiting for condition variable */
#define WAIT_SEMAPHORE  0x0008  /* Waiting for semaphore */
#define WAIT_SIGNAL     0x0010  /* Waiting for signal */
#define WAIT_IO         0x0020  /* Waiting for I/O */
#define WAIT_SLEEP      0x0040  /* Sleeping */
#define WAIT_OTHER      0x0080  /* Other wait reason */

/* ============================================================================
 * P_THREAD_SIGNAL OPERATIONS (sys_p_thread_signal)
 * ============================================================================ */
#define PTSIG_MODE              0   /* Enable/disable thread signals */
#define PTSIG_KILL              1   /* Send signal to specific thread */
#define PTSIG_GETMASK           2   /* Get thread signal mask */
#define PTSIG_SETMASK           3   /* Set thread signal mask */
#define PTSIG_BLOCK             4   /* Block signals */
#define PTSIG_UNBLOCK           5   /* Unblock signals */
#define PTSIG_WAIT              6   /* Wait for signal */
#define PTSIG_HANDLER           7   /* Set signal handler */
#define PTSIG_HANDLER_ARG       8   /* Set signal handler with argument */
#define PTSIG_PENDING           9   /* Get pending signals */
#define PTSIG_ALARM             10   /* Set alarm signal */
#define PTSIG_ALARM_THREAD      11   /* Set alarm signal for specific thread */
#define PTSIG_PAUSE             12   /* Pause with specified mask */
#define PTSIG_BROADCAST         13   /* Broadcast signal to all threads */

#define PTSIG_WAITINFO          14   /* sigwaitinfo() */
#define PTSIG_TIMEDWAIT         15   /* sigtimedwait() */
#define PTSIG_QUEUE             16   /* sigqueue() */

#define PTSIG_EXT_HANDLER       17   /* Set extended signal handler with siginfo */

/* ============================================================================
 * PTHREAD COMPATIBILITY CONSTANTS
 * ============================================================================ */

/* Thread Cancellation */
#define PTHREAD_CANCEL_ENABLE       0   /* Enable cancellation */
#define PTHREAD_CANCEL_DISABLE      1   /* Disable cancellation */
#define PTHREAD_CANCEL_DEFERRED     0   /* Deferred cancellation */
#define PTHREAD_CANCEL_ASYNCHRONOUS 1   /* Asynchronous cancellation */
#define PTHREAD_CANCELED           ((void *)-1)  /* Canceled thread return value */

/* Mutex Types (for mutexattr) */
#define PTHREAD_MUTEX_NORMAL        0   /* Normal mutex */
#define PTHREAD_MUTEX_RECURSIVE     1   /* Recursive mutex */
#define PTHREAD_MUTEX_ERRORCHECK    2   /* Error-checking mutex */

/* Mutex Protocols (for priority inheritance) */
#define PTHREAD_PRIO_NONE           0   /* No priority inheritance */
#define PTHREAD_PRIO_INHERIT        1   /* Priority inheritance */
#define PTHREAD_PRIO_PROTECT        2   /* Priority ceiling protocol */

/* ============================================================================
 * SYSTEM CONSTANTS
 * ============================================================================ */
#define THREAD_SUCCESS              0   /* Operation successful */
#define MS_PER_TICK                 5   /* Milliseconds per system tick (200Hz) */
#define MAX_SWITCH_RETRIES          3   /* Maximum context switch retry attempts */

/* 
 * Thread priority scaling:
 * - User-facing API accepts priorities in the standard POSIX range (0-99)
 * - Internally, priorities are scaled to 0-16 range when set via syscalls
 * - This allows efficient bitmap operations while maintaining POSIX compatibility
 * - Scaling uses fast multiply-shift: (priority * 10923) >> 16 ≈ (priority * 16) / 99
 */

#define MAX_POSIX_THREAD_PRIORITY  99   /* Maximum POSIX thread priority */
#define MIN_POSIX_THREAD_PRIORITY   1   /* Minimum POSIX thread priority */

#define MAX_THREAD_PRIORITY        16   /* Internal maximum thread priority */
#define MIN_THREAD_PRIORITY         0   /* Internal minimum thread priority */

#define THREAD_CREATION_PRIORITY_BOOST 3   /* Priority boost for new threads */

// #define DEFAULT_SCHED_POLICY   SCHED_FIFO   /* Default scheduling policy */
#define DEFAULT_SCHED_POLICY   SCHED_RR   /* Default scheduling policy */
/* Current thread macro */
#define CURTHREAD \
    ((curproc && curproc->current_thread) ? curproc->current_thread : NULL)

/* ============================================================================
 * FUNCTION PROTOTYPES
 * ============================================================================ */
long proc_thread_status(long tid); /* Get thread status */
CONTEXT* get_thread_context(struct thread *t); /* Get thread context */
struct thread* get_idle_thread(struct proc *p);	/* Get idle thread */
struct thread* get_main_thread(struct proc *p);	/* Get main thread */

#endif /* PROC_THREAD_H */
