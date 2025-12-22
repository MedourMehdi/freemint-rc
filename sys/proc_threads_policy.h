/**
 * @file proc_threads_policy.h
 * @brief Thread Scheduling Policy Interface
 * 
 * Declares POSIX scheduling policy control and timeslice management APIs.
 * Defines structures for policy enforcement across SCHED_FIFO, SCHED_RR, 
 * and SCHED_OTHER scheduling classes.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#ifndef PROC_THREADS_POLICY_H
#define PROC_THREADS_POLICY_H

#include "proc_threads.h"

/* Update thread timeslice accounting during context switches */
void update_thread_timeslice(struct thread *t);

/* Thread scheduling system calls */
long proc_thread_get_timeslice(long tid, long *timeslice, long *remaining);
long proc_thread_set_timeslice(long tid, long timeslice);
long proc_thread_get_rrinterval(long tid, long *interval);
long proc_thread_get_schedparam(long tid, long *policy, long *priority);
long proc_thread_set_schedparam(long tid, long policy, long priority);

/* Policy configuration */
long proc_thread_set_policy(enum sched_policy policy, short priority, short timeslice);

#endif //PROC_THREADS_POLICY_H