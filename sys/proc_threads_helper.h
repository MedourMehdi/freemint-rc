/**
 * @file proc_threads_helper.h
 * @brief Threading Utility Functions
 * 
 * Declares helper functions for kernel threading subsystem including:
 *  - Priority bitmap operations
 *  - Thread state management
 *  - Scheduling utilities
 *  - System tick access
 * 
 * Provides fast lookup tables and atomic operations optimized for m68000.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#ifndef PROC_THREADS_HELPER_H
#define PROC_THREADS_HELPER_H

#include "proc_threads.h"

long sys_p_thread_getid(void);
void proc_thread_state_change(struct thread *t, int new_state);
struct thread *get_highest_priority_thread(struct proc *p);
struct thread *get_highest_priority_thread_excluding(struct proc *p, struct thread *exclude);
unsigned long get_system_ticks(void);
void make_process_eligible(struct proc *p);
void boost_thread_priority(struct thread *t, int boost_amount);
void reset_thread_priority(struct thread *t);
long timeout_remaining(TIMEOUT *t);
void update_thread_cpu_time(struct thread *t);
void reset_thread_cpu_time(struct thread *t);

/* Lookup tables - must be extern for inline functions */
extern const unsigned char priority_scale_table[100];

inline int scale_thread_priority(int priority);

struct thread *proc_thread_find(struct proc *p, short tid);

#endif //PROC_THREADS_HELPER_H