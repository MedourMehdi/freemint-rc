/**
 * @file proc_threads_mutex.h
 * @brief Thread Synchronization Primitives Interface
 * 
 * Declares POSIX synchronization mechanisms and thread lifecycle operations.
 * Defines structures and APIs for mutexes, condition variables, semaphores,
 * and thread joining/detaching operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"

#ifndef PROC_THREADS_MUTEX_H
#define PROC_THREADS_MUTEX_H

/* Mutex attribute structure */
struct mutex_attr {
    short type;                 /* Mutex type: NORMAL, RECURSIVE, ERRORCHECK */
    short pshared;              /* Process-shared flag */
    short protocol;             /* Priority protocol */
    short prioceiling;          /* Priority ceiling value */
};

struct mutex {
    struct thread *owner;
    struct thread *wait_queue;    
    short locked;
    short protocol;              /* Priority protocol */
    short type;                  /* Mutex type */
    short prioceiling;           /* Priority ceiling */
    short saved_priority;        /* Saved priority for ceiling protocol */
    short lock_count;            /* Lock count for recursive mutexes */
};

int thread_mutex_trylock(struct mutex *mutex);
// Function to unlock a mutex
int thread_mutex_unlock(struct mutex *mutex);
// Function to lock a mutex
int thread_mutex_lock(struct mutex *mutex);
// Function to initialize a mutex
int thread_mutex_init(struct mutex *mutex, const struct mutex_attr *attr);
// Function to destroy a mutex
int thread_mutex_destroy(struct mutex *mutex);
// Function to initialize mutex attributes
int thread_mutexattr_init(struct mutex_attr *attr);
// Function to destroy mutex attributes
int thread_mutexattr_destroy(struct mutex_attr *attr);

int thread_mutex_wait(struct mutex *mutex);
int thread_mutex_wake(struct mutex *mutex);

#endif /* PROC_THREADS_MUTEX_H */