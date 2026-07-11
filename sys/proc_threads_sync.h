/**
 * @file proc_threads_sync.h
 * @brief Thread Synchronization Primitives Interface
 * 
 * Declares POSIX synchronization mechanisms and thread lifecycle operations.
 * Defines structures and APIs for condition variables, semaphores,
 * and thread joining/detaching operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"
#include "proc_threads_mutex.h"
#include "proc_threads_cond.h"

#ifndef PROC_THREADS_SYNC_H
#define PROC_THREADS_SYNC_H

/* Read-Write Lock Structure */
struct rwlock {
    struct mutex lock;          // Mutex protecting internal state
    struct condvar readers_ok;  // Readers condition variable
    struct condvar writers_ok;  // Writers condition variable
    int readers;                // Active readers count
    int writers;                // Active writers (0 or 1)
    int waiting_writers;        // Writers waiting for access
    int waiting_readers;        // Readers waiting for access
};

long proc_thread_join(long tid, void **retval);
long proc_thread_detach(long tid);

/* Function to clean up thread synchronization states */
void cleanup_thread_sync_states(struct proc *p);

long thread_rwlock_init(void);
long thread_rwlock_destroy(long handle);
long thread_rwlock_rdlock(long handle);
long thread_rwlock_tryrdlock(long handle);
long thread_rwlock_wrlock(long handle);
long thread_rwlock_trywrlock(long handle);
long thread_rwlock_unlock(long handle);

#endif /* PROC_THREADS_SYNC_H */