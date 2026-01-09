/**
 * @file proc_threads_atomic.c
 * @brief Atomic Operations for FreeMiNT Threading System
 * 
 * Provides atomic operations for the threading subsystem.
 * These operations are implemented as kernel functions since they require
 * supervisor mode access to manipulate the status register.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"
#include "proc_threads_atomic.h"
#include "proc_threads_sleep_yield.h"
#include "mint/arch/asm_spl.h"

/* ============================================================================
 * LOW-LEVEL TAS IMPLEMENTATION
 * ============================================================================ */

/**
 * M68K TAS instruction implementation with ColdFire support
 * 
 * TAS (Test and Set) is an atomic read-modify-write operation:
 * 1. Tests if memory location is 0
 * 2. Sets it to 0xFF (all bits set)
 * 3. Returns the original value in the Z flag
 * 
 * Z=1 means it was 0 (unlocked) - we acquired the lock
 * Z=0 means it was non-zero (locked) - acquisition failed
 */

#ifdef __mcoldfire__
/* ColdFire version - limited instruction set */
int tas_try_lock(volatile unsigned short *lock_word) {
    register unsigned char result;
    register unsigned short sr = splhigh();
    __asm__ volatile (
        "tas %1\n\t"        /* Test and set high byte of word */
        "beq 1f\n\t"        /* If Z=1 (was 0) -> acquired lock */
        "moveq #0,%0\n\t"   /* Failed: result = 0 */
        "bra 2f\n\t"
        "1:\n\t"
        "moveq #1,%0\n\t"   /* Success: result = 1 */
        "2:\n\t"
        : "=d" (result), "+m" (*lock_word)
        :
        : "cc"
    );
    spl(sr);
    return result;
}
#else
/* Standard m68k version - single-cycle TAS */
int tas_try_lock(volatile unsigned short *lock_word) {
    register unsigned char result;
    __asm__ volatile (
        "tas %1\n\t"        /* Test and set high byte of word */
        "seq %0\n\t"        /* Set byte to $FF if Z=1 (was unlocked) */
        "negb %0\n\t"       /* Convert $FF->1, $00->0 */
        "andb #1,%0"        /* Ensure clean boolean 0 or 1 */
        : "=d" (result), "+m" (*lock_word)
        :
        : "cc"
    );

    return result;
}
#endif

/**
 * Unlock - Clear entire word for safety
 * CLR.W is cleaner than CLR.B for partial word updates
 */
void tas_unlock(volatile unsigned short *lock_word) {
    /* Clear entire word - 4 cycles on m68k, atomic and visible */
    __asm__ volatile ("clr %0" : "+m" (*lock_word) :: "cc");
}

/* ============================================================================
 * ATOMIC OPERATIONS USING INTERRUPT DISABLE
 * ============================================================================ */
inline int atomic_increment(volatile long *value) {
    int result;
    register unsigned short sr = splhigh();
    result = ++(*value);
    spl(sr);
    return result;
}

inline int atomic_decrement(volatile long *value) {
    int result;
    register unsigned short sr = splhigh();
    result = --(*value);
    spl(sr);
    return result;
}

inline int atomic_cas(volatile long *ptr, long oldval, long newval) {
    int result;
    register unsigned short sr = splhigh();
    if (*ptr == oldval) {
        TRACE_THREAD("atomic_cas: Success, oldval=%ld, newval=%ld\n", oldval, newval);
        *ptr = newval;
        result = 1;  /* Success - old value matched */
    } else {
        TRACE_THREAD("atomic_cas: Failure, oldval=%ld, newval=%ld\n", oldval, newval);
        result = 0;  /* Failure - old value didn't match */
    }
    spl(sr);
    return result;
}

inline int atomic_exchange(volatile long *ptr, long newval) {
    int oldval;
    register unsigned short sr = splhigh();
    oldval = *ptr;
    *ptr = newval;
    spl(sr);
    return oldval;
}

inline int atomic_add(volatile long *ptr, long value) {
    int result;
    register unsigned short sr = splhigh();
    result = (*ptr) + value;
    *ptr = result;
    spl(sr);
    return result;
}

inline int atomic_sub(volatile long *ptr, long value) {
    int result;
    register unsigned short sr = splhigh();
    result = (*ptr) - value;
    *ptr = result;
    spl(sr);
    return result;
}

inline int atomic_or(volatile long *ptr, long value) {
    int result;
    register unsigned short sr = splhigh();
    result = (*ptr) | value;
    *ptr = result;
    spl(sr);
    return result;
}

inline int atomic_and(volatile long *ptr, long value) {
    int result;
    register unsigned short sr = splhigh();
    result = (*ptr) & value;
    *ptr = result;
    spl(sr);
    return result;
}

inline int atomic_xor(volatile long *ptr, long value) {
    int result;
    register unsigned short sr = splhigh();
    result = (*ptr) ^ value;
    *ptr = result;
    spl(sr);
    return result;
}

inline void atomic_counter_init(atomic_counter_t *counter, int initial_value) {
    counter->counter = initial_value;
}

inline int atomic_counter_inc(atomic_counter_t *counter) {
    return thread_atomic_increment(&counter->counter);
}

inline int atomic_counter_dec(atomic_counter_t *counter) {
    return thread_atomic_decrement(&counter->counter);
}

inline int atomic_counter_get(atomic_counter_t *counter) {
    return ATOMIC_GET(counter->counter);
}

/* Thread-safe linked list operations */
int thread_atomic_list_add(struct thread **head, struct thread *new_thread) {
    if (!head || !new_thread || new_thread->magic != CTXT_MAGIC) {
        return EINVAL;
    }
    
    register unsigned short sr = splhigh();
    new_thread->next = *head;
    *head = new_thread;
    spl(sr);
    
    return 0;
}

int thread_atomic_list_remove(struct thread **head, struct thread *thread_to_remove) {
    if (!head || !thread_to_remove || thread_to_remove->magic != CTXT_MAGIC) {
        return EINVAL;
    }
    
    register unsigned short sr = splhigh();
    struct thread *current = *head;
    struct thread *prev = NULL;
    
    while (current) {
        if (current == thread_to_remove) {
            if (prev) {
                prev->next = current->next;
            } else {
                *head = current->next;
            }
            current->next = NULL;
            spl(sr);
            return 0;
        }
        prev = current;
        current = current->next;
    }
    
    spl(sr);
    return ESRCH;  /* Thread not found in list */
}

/* ============================================================================
 * SPINLOCK IMPLEMENTATION (Built on TAS)
 * ============================================================================ */

inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->owner_tid = -1;
}

inline void spinlock_lock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    short tid = t ? t->tid : -1;

    /* Spin using TAS until we acquire the lock */
    /* Use yielding spinlock to avoid burning CPU cycles */
    while (!tas_try_lock(&lock->locked)) {
        /* Small delay before retry to reduce bus contention */
        MEMORY_BARRIER();        
        proc_thread_yield();
    }
    
    lock->owner_tid = tid;
    MEMORY_BARRIER();
}

inline int spinlock_trylock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    short tid = t ? t->tid : -1;
    
    if (tas_try_lock(&lock->locked)) {
        lock->owner_tid = tid;
        MEMORY_BARRIER();
        return 1;  /* Success */
    }
    return 0;  /* Failed */
}

inline void spinlock_unlock(spinlock_t *lock) {
    MEMORY_BARRIER();
    lock->owner_tid = -1;
    tas_unlock(&lock->locked);
}