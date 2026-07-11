/**
 * @file proc_threads_sem.h
 * @brief Thread Semaphore Synchronization Primitives Interface
 * 
 * Declares POSIX semaphore synchronization mechanisms
 * Defines structures and APIs for semaphores
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"

#ifndef PROC_THREADS_SEM_H
#define PROC_THREADS_SEM_H

#define SEM_NAME_MAX    4

struct semaphore {
    struct thread *wait_queue;      /* Queue of threads waiting on this sem */
    volatile long count;           /* Current semaphore count */
    /* Non threaded values */
    volatile long io_count;        /* Reference count for named sems */
    char sem_id[SEM_NAME_MAX + 1];  /* Fixed array instead of pointer */
};

long thread_semaphore_up(struct semaphore *sem);
long thread_semaphore_down(struct semaphore *sem);
long thread_semaphore_init(struct semaphore *sem);

long thread_semaphore_timeddown(struct semaphore *sem, long ms);
long thread_semaphore_trydown(struct semaphore *sem);

#endif //PROC_THREADS_SEM_H