/**
 * @file proc_threads_syscall.h
 * @brief 
 * 
 * 
 * 
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#ifndef _PROC_THREADS_SYSCALL_H
#define _PROC_THREADS_SYSCALL_H

#define P_PTHREAD 0x185

/* ============================================================================
 * PRIMARY SYSCALL CATEGORIES (sys_p_thread_syscall dispatcher)
 * ============================================================================ */
#define P_THREAD_CTRL       1   /* Thread control operations */
#define P_THREAD_SYNC       2   /* Synchronization operations */
#define P_THREAD_SIGNAL     3   /* Signal operations */

/* Scheduling policy operations (handled directly by dispatcher) */
#define PSCHED_SETPARAM         11   /* Set thread scheduling parameters */
#define PSCHED_GETPARAM         12   /* Get thread scheduling parameters */
#define PSCHED_GETRRINTERVAL    13   /* Get round-robin time slice interval */
#define PSCHED_SET_TIMESLICE    14   /* Set thread time slice */
#define PSCHED_GET_TIMESLICE    15   /* Get thread time slice */

/* Atomic operations (handled directly by dispatcher) */
#define THREAD_ATOMIC_INCREMENT     21   /* Atomic increment */
#define THREAD_ATOMIC_DECREMENT     22   /* Atomic decrement */
#define THREAD_ATOMIC_CAS           23   /* Compare-and-swap */
#define THREAD_ATOMIC_EXCHANGE      24   /* Atomic exchange */
#define THREAD_ATOMIC_ADD           25   /* Atomic addition */
#define THREAD_ATOMIC_SUB           26   /* Atomic subtraction */
#define THREAD_ATOMIC_OR            27   /* Atomic bitwise OR */
#define THREAD_ATOMIC_AND           28   /* Atomic bitwise AND */
#define THREAD_ATOMIC_XOR           29   /* Atomic bitwise XOR */
#define THREAD_ATOMIC_TAS           30   /* Test-and-set */

/* ============================================================================
 * P_THREAD_CTRL OPERATIONS (sys_p_thread_ctrl)
 * ============================================================================ */
#define THREAD_CTRL_EXIT                0   /* Exit the current thread */
#define THREAD_CTRL_CANCEL              1   /* Cancel a thread */
#define THREAD_CTRL_STATUS              4   /* Get thread status */
#define THREAD_CTRL_GETID               5   /* Get thread ID */
#define THREAD_CTRL_SETCANCELSTATE      6   /* Set cancellation state */
#define THREAD_CTRL_SETCANCELTYPE       7   /* Set cancellation type */
#define THREAD_CTRL_TESTCANCEL          8   /* Test for pending cancellation */
#define THREAD_CTRL_SETNAME             9   /* Set thread name */
#define THREAD_CTRL_GETNAME            10   /* Get thread name */
#define THREAD_CTRL_IS_INITIAL         13   /* Check if current thread is initial */
#define THREAD_CTRL_IS_MULTITHREADED   14   /* Check if process is multithreaded */
#define THREAD_CTRL_SWITCH_TO_MAIN     15   /* Switch to main thread context */
#define THREAD_CTRL_SWITCH_TO_THREAD   16   /* Switch to specific thread */

/* ============================================================================
 * P_THREAD_SYNC OPERATIONS (sys_p_thread_sync)
 * ============================================================================
 * Operation Number Ranges:
 * - Semaphore Operations: 1-3
 * - Mutex Operations: 10-14
 * - Mutex Attribute Operations: 20-27
 * - Condition Variable Operations: 30-35
 * - Reader-Writer Lock Operations: 40-46
 * - Thread Lifecycle Operations: 50-52
 * - Thread Scheduling Operations: 60-61
 * - Cleanup Handler Operations: 70-72
 * - Thread-Specific Data Operations: 80-83
 * ============================================================================ */

/* --- Semaphore Operations --- */
#define THREAD_SYNC_SEM_WAIT            1   /* Wait on semaphore (P operation) */
#define THREAD_SYNC_SEM_POST            2   /* Signal semaphore (V operation) */
#define THREAD_SYNC_SEM_INIT            3   /* Initialize semaphore */

/* --- Mutex Operations --- */
#define THREAD_SYNC_MUTEX_INIT          10  /* Initialize mutex */
#define THREAD_SYNC_MUTEX_LOCK          11  /* Lock mutex */
#define THREAD_SYNC_MUTEX_UNLOCK        12  /* Unlock mutex */
#define THREAD_SYNC_MUTEX_TRYLOCK       13  /* Try to lock mutex (non-blocking) */
#define THREAD_SYNC_MUTEX_DESTROY       14  /* Destroy mutex */

/* --- Mutex Attribute Operations --- */
#define THREAD_SYNC_MUTEX_ATTR_INIT        20  /* Initialize mutex attributes */
#define THREAD_SYNC_MUTEX_ATTR_DESTROY     21  /* Destroy mutex attributes */
#define THREAD_SYNC_MUTEXATTR_SETTYPE      22  /* Set mutex type */
#define THREAD_SYNC_MUTEXATTR_GETTYPE      23  /* Get mutex type */
#define THREAD_SYNC_MUTEXATTR_SETPROTOCOL  24  /* Set mutex protocol */
#define THREAD_SYNC_MUTEXATTR_GETPROTOCOL  25  /* Get mutex protocol */
#define THREAD_SYNC_MUTEXATTR_SETPRIOCEILING 26 /* Set priority ceiling */
#define THREAD_SYNC_MUTEXATTR_GETPRIOCEILING 27 /* Get priority ceiling */

/* --- Condition Variable Operations --- */
#define THREAD_SYNC_COND_INIT          30  /* Initialize condition variable */
#define THREAD_SYNC_COND_DESTROY       31  /* Destroy condition variable */
#define THREAD_SYNC_COND_WAIT          32  /* Wait on condition variable */
#define THREAD_SYNC_COND_TIMEDWAIT     33  /* Timed wait on condition variable */
#define THREAD_SYNC_COND_SIGNAL        34  /* Signal condition variable */
#define THREAD_SYNC_COND_BROADCAST     35  /* Broadcast to condition variable */

/* --- Reader-Writer Lock Operations --- */
#define THREAD_SYNC_RWLOCK_INIT        40  /* Initialize reader-writer lock */
#define THREAD_SYNC_RWLOCK_DESTROY     41  /* Destroy reader-writer lock */
#define THREAD_SYNC_RWLOCK_RDLOCK      42  /* Acquire read lock */
#define THREAD_SYNC_RWLOCK_WRLOCK      43  /* Acquire write lock */
#define THREAD_SYNC_RWLOCK_UNLOCK      44  /* Release reader-writer lock */
#define THREAD_SYNC_RWLOCK_TRYRDLOCK   45  /* Try to acquire read lock */
#define THREAD_SYNC_RWLOCK_TRYWRLOCK   46  /* Try to acquire write lock */

/* --- Thread Lifecycle Operations --- */
#define THREAD_SYNC_JOIN               50  /* Join thread and wait for termination */
#define THREAD_SYNC_DETACH             51  /* Detach thread (make unjoinable) */
#define THREAD_SYNC_TRYJOIN            52  /* Non-blocking join attempt */

/* --- Thread Scheduling Operations --- */
#define THREAD_SYNC_SLEEP              60  /* Sleep for specified milliseconds */
#define THREAD_SYNC_YIELD              61  /* Yield CPU to other threads */

/* --- Cleanup Handler Operations --- */
#define THREAD_SYNC_CLEANUP_PUSH       70  /* Push cleanup handler */
#define THREAD_SYNC_CLEANUP_POP        71  /* Pop cleanup handler */
#define THREAD_SYNC_CLEANUP_GET        72  /* Get cleanup handlers */

/* --- Thread-Specific Data Operations --- */
#define THREAD_TSD_CREATE_KEY          80  /* Create TSD key */
#define THREAD_TSD_DELETE_KEY          81  /* Delete TSD key */
#define THREAD_TSD_GET_SPECIFIC        82  /* Get thread-specific data */
#define THREAD_TSD_SET_SPECIFIC        83  /* Set thread-specific data */

long _cdecl sys_p_thread_ctrl(long func, long arg1, long arg2);
long _cdecl sys_p_thread_signal(long func, long arg1, long arg2);
long _cdecl sys_p_thread_sync(long operator, long arg1, long arg2);
long _cdecl sys_p_pthread(long syscall_func, long arg1, long arg2, long arg3);

#endif /* _PROC_THREADS_SYSCALL_H */