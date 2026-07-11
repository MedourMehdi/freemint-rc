#include "proc_threads_sem.h"

#include "proc_threads_scheduler.h"
#include "proc_threads_helper.h"
#include "proc_threads_queue.h"

static void proc_thread_sem_wakeup_handler(PROC *p, long arg);
static long semaphore_handle_signal_wakeup(struct thread *t, struct semaphore *sem);

// Function to initialize a semaphore
long thread_semaphore_init(struct semaphore *sem) {
    
    if (!sem) {
        return EINVAL;
    }

    if (sem->count < 0) {
        return EINVAL;
    }    

    sem->wait_queue = NULL;

    TRACE_THREAD("SEMAPHORE INIT: Count=%ld", sem->count);

    return THREAD_SUCCESS;
}

/* Handler exécuté par le système de timer du noyau quand le délai expire */
static void proc_thread_sem_wakeup_handler(PROC *p, long arg) {
    struct thread *t = (struct thread *)arg;
    struct semaphore *sem;
    struct thread **pp;

    if (!t || t->magic != CTXT_MAGIC) return;

    TRACE_THREAD("SEMAPHORE TIMEOUT: Thread=%d", t->tid);
    /* Si le thread est toujours en attente du sémaphore (il n'a pas été réveillé par sem_up) */
    if (t->wait_type & WAIT_SEMAPHORE) {
        sem = (struct semaphore *)t->sem_wait_obj;
        if (sem) {
            /* Retrait propre de la file d'attente du sémaphore */
            pp = &sem->wait_queue;
            while (*pp) {
                if (*pp == t) {
                    *pp = t->next_wait;
                    break;
                }
                pp = &(*pp)->next_wait;
            }
        }
        
        TRACE_THREAD("SEMAPHORE TIMEOUT: Thread=%d, sem=%p", t->tid, sem);
        /* Nettoyage et marquage spécifique pour indiquer le Timeout au thread */
        t->wait_type &= ~WAIT_SEMAPHORE;
        t->sem_wait_obj = (void *)-1L; /* Valeur magique signalant un ETIMEDOUT */
        t->next_wait = NULL;
        t->sleep_timeout = NULL;

        /* Réveil du thread */
        proc_thread_state_change(t, THREAD_STATE_READY);
        add_to_ready_queue(t);

        if (curproc == p) {
            proc_thread_schedule();
        }
    }
}

/**
 * Handle signal interruption after waking from semaphore wait.
 * Returns THREAD_SUCCESS if normal wakeup, EINTR if signal interrupted.
 */
static long semaphore_handle_signal_wakeup(struct thread *t, struct semaphore *sem)
{
    register unsigned short sr;
    struct thread **pp;

    /* Robust signal detection: sem_up and timeout both clear this.
     * If still set, we were woken by a signal.
     */
    if (!(t->wait_type & WAIT_SEMAPHORE))
        return THREAD_SUCCESS;

    sr = splhigh();

    /* Remove ourselves from the semaphore wait queue if still linked.
     * We must do this atomically because sem_up may be scanning the queue.
     */
    if (sem && t->sem_wait_obj == sem) {
        pp = &sem->wait_queue;
        while (*pp) {
            if (*pp == t) {
                *pp = t->next_wait;
                break;
            }
            pp = &(*pp)->next_wait;
        }
    }

    t->next_wait    = NULL;
    t->wait_type   &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj = NULL;

    /* Cancel any pending timeout — the signal took precedence */
    if (t->sleep_timeout) {
        canceltimeout(t->sleep_timeout);
        t->sleep_timeout = NULL;
    }

    spl(sr);

    TRACE_THREAD("SEM: Thread %d interrupted by signal on sem=%p", t->tid, sem);
    return EINTR;
}

/* =========================================================================
 * thread_semaphore_trydown  —  sem_trywait (100% Non-Bloquant)
 * =========================================================================
 */
long thread_semaphore_trydown(struct semaphore *sem)
{
    struct proc *p = curproc;
    struct thread *t = p ? p->current_thread : NULL;

    if (!sem || !t || t->magic != CTXT_MAGIC) return EINVAL;

    TRACE_THREAD("SEMAPHORE TRYDOWN: Count=%ld", sem->count);
    if (sem->count > 0) {
        sem->count--;
        return THREAD_SUCCESS; /* 0 */
    }

    return EAGAIN; /* Le sémaphore est à 0, on retourne l'erreur immédiatement */
}

/* =========================================================================
 * thread_semaphore_timeddown  —  sem_timedwait
 * =========================================================================
 */
long thread_semaphore_timeddown(struct semaphore *sem, long ms)
{
    struct proc            *p = curproc;
    struct thread          *t = p ? p->current_thread : NULL;
    struct thread          *iter;
    CONTEXT       *ctx;

    if (!sem || !t || t->magic != CTXT_MAGIC) return EINVAL;

    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Count=%ld, ms=%ld", sem->count, ms);
    /* Un timeout nul ou négatif est un trywait déguisé */
    if (ms <= 0) {
        return thread_semaphore_trydown(sem);
    }

    /* --- CONTRAINTE ARCHITECTURALE : Thread 0 --- */
    if (t->tid == 0) {
        unsigned long start_ticks = get_system_ticks();
        /* Conversion grossière : 1 tick système = 5ms dans FreeMiNT */
        unsigned long wait_ticks = (ms + 4) / 5; 
        
        for (;;) {
            yield();
            if (sem->count > 0) {
                sem->count--;
                return THREAD_SUCCESS;
            }
            
            /* Vérification du dépassement de délai */
            if ((get_system_ticks() - start_ticks) >= wait_ticks) {
                return ETIMEDOUT;
            }
        }
    }

    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Thread=%d, sem=%p", t->tid, sem);
    /* --- LOGIQUE DES THREADS > 0 --- */
    if (t->wait_type != WAIT_NONE) return EDEADLK;

    if (sem->count > 0) {
        sem->count--;
        return THREAD_SUCCESS;
    }

    /* Enqueueing */
    t->next_wait    = NULL;
    t->wait_type    = WAIT_SEMAPHORE;
    t->sem_wait_obj = sem;

    if (!sem->wait_queue) {
        sem->wait_queue = t;
    } else {
        iter = sem->wait_queue;
        while (iter->next_wait) iter = iter->next_wait;
        iter->next_wait = t;
    }

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    remove_from_ready_queue(t);

    /* --- ARMEMENT DU TIMEOUT --- */
    t->sleep_timeout = addtimeout(p, ms, proc_thread_sem_wakeup_handler);

    if (t->sleep_timeout) {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Timeout=%p", t->sleep_timeout);
        t->sleep_timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Timeout allocation failed");
        /* Fallback si le noyau n'a plus de RAM pour allouer un timer */
        t->wait_type &= ~WAIT_SEMAPHORE;
        t->sem_wait_obj = NULL;
        proc_thread_state_change(t, THREAD_STATE_RUNNING);
        return ENOMEM;
    }

    ctx = get_thread_context(t);
    /* Sauvegarde et Bascule (Pattern A) */
    if (save_context(ctx) == 0) {
        TRACE_THREAD("SEMAPHORE TIMEDDOWN: Sauvegarde de contexte OK");
        ctx->regs[0] = 1;
        proc_thread_schedule();
        TRACE_THREAD("FATAL: Retour inattendu de proc_thread_schedule !");
        return -1;
    }

    // proc_thread_schedule();

    TRACE_THREAD("SEMAPHORE TIMEDDOWN: Reload de contexte");
    /* --- RÉVEIL --- */
    if (t->state != THREAD_STATE_RUNNING) {
        proc_thread_state_change(t, THREAD_STATE_RUNNING);
    }

    /* Check for signal interruption first */
    if (semaphore_handle_signal_wakeup(t, sem) == EINTR) {
        return EINTR;
    }

    /* On détermine qui nous a réveillé : le Waker ou le Timeout ? */
    int ret = THREAD_SUCCESS;
    
    if (t->sem_wait_obj == (void *)-1L) {
        /* Le handler de Timeout a laissé cette signature */
        t->sem_wait_obj = NULL;
        ret = ETIMEDOUT;
    } else {
        /* Réveillé proprement par sem_up. Le timer a DEJA été annulé par sem_up. */
        ret = THREAD_SUCCESS;
    }
    return ret;
}

/* =========================================================================
 * thread_semaphore_down  (sem_wait equivalent)
 * =========================================================================
 *
 * Calling paths
 * -------------
 *   tid > 0  →  standard blocking path using save_context / change_context
 *   tid == 0 →  spin-yield path (no context save, never enters wait_queue)
 *
 * Returns THREAD_SUCCESS (0) on acquisition, or a positive errno on error.
 */
long thread_semaphore_down(struct semaphore *sem)
{
    struct proc   *p;
    struct thread *t;
    CONTEXT       *ctx;
    register unsigned short sr;
 
    if (!sem) {
        TRACE_THREAD("SEM_DOWN: NULL semaphore pointer");
        return EINVAL;
    }
 
    p = curproc;
    t = p ? p->current_thread : NULL;
 
    if (!t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("SEM_DOWN: No valid current thread");
        return EINVAL;
    }
 
    TRACE_THREAD("SEM_DOWN: tid=%d sem=%p count=%d", t->tid, sem, sem->count);
 
    /* ------------------------------------------------------------------
     * Fast path: count already > 0 — decrement and return immediately.
     * ------------------------------------------------------------------ */
    sr = splhigh();
    if (sem->count > 0) {
        sem->count--;
        TRACE_THREAD("SEM_DOWN: Fast-path acquired, count now %ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }
    spl(sr);
 
    /* ------------------------------------------------------------------
     * Slow path: semaphore exhausted.
     * ------------------------------------------------------------------ */
 
    /* --- tid == 0: spin-yield, never block via save_context ----------- */
    if (t->tid == 0) {
        TRACE_THREAD("SEM_DOWN: tid=0 spin-yield loop");
        while (1) {
            yield();                    /* FreeMiNT process yield            */
            sr = splhigh();
            if (sem->count > 0) {
                sem->count--;
                TRACE_THREAD("SEM_DOWN: tid=0 acquired after yield, count=%ld",
                             sem->count);
                spl(sr);
                return THREAD_SUCCESS;
            }
            spl(sr);
        }
        /* NOTREACHED */
    }
 
    /* --- tid > 0: proper blocking path -------------------------------- */
 
    /* Prevent re-entrant blocking on the same thread */
    if (t->wait_type != WAIT_NONE) {
        TRACE_THREAD("SEM_DOWN: Thread %d already blocked (wait_type=0x%x)",
                     t->tid, t->wait_type);
        return EDEADLK;
    }
 
    sr = splhigh();
 
    /*
     * Re-check under interrupt lock: another thread may have called
     * sem_up between our first check and splhigh().
     */
    if (sem->count > 0) {
        sem->count--;
        TRACE_THREAD("SEM_DOWN: Re-check fast-path acquired, count=%ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }
 
    /* Append to FIFO wait queue */
    t->next_wait      = NULL;
    t->wait_type      |= WAIT_SEMAPHORE;
    t->sem_wait_obj   = sem;

    proc_thread_state_change(t, THREAD_STATE_BLOCKED);
    remove_from_ready_queue(t);     /* ensure not on ready queue            */

    if (!sem->wait_queue) {
        sem->wait_queue = t;
    } else {
        struct thread *iter = sem->wait_queue;
        while (iter->next_wait)
            iter = iter->next_wait;
        iter->next_wait = t;
    }
 
    spl(sr);
 
    TRACE_THREAD("SEM_DOWN: Thread %d blocking on sem=%p", t->tid, sem);
 
    /* ------------------------------------------------------------------
     * Save context.  On first call (returns 0) we hand off to the
     * scheduler; we come back here when sem_up restores our context
     * (save_context returns 1 because sem_up sets regs[0]=1).
     * ------------------------------------------------------------------ */
    ctx = get_thread_context(t);
 
    if (save_context(ctx) == 0) {
        /* First call: context is now saved — go to sleep. */
        ctx->regs[0] = 1;           /* wake-up discriminator for restore    */
 
        TRACE_THREAD("SEM_DOWN: SAVED SYSCALL CONTEXT: Thread %d context - SR=%x, SSP=%lx, USP=%lx, PC=%lx", t->tid, t->ctxt[SYSCALL].sr, t->ctxt[SYSCALL].ssp, t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].pc);
        proc_thread_schedule();
 
        /*
         * Should never be reached: proc_thread_schedule() performs
         * change_context() to another thread and never returns on this
         * code path.
         */
        TRACE_THREAD("SEM_DOWN: ERROR — returned from proc_thread_schedule()!");
        return -1;
    }

    TRACE_THREAD("SEM_DOWN: Second time context, THREAD SYSCALL context for thread %d SR = %x, SSP=%lx, USP=%lx, PC=%lx", t->tid, t->ctxt[SYSCALL].sr, t->ctxt[SYSCALL].ssp, t->ctxt[SYSCALL].usp, t->ctxt[SYSCALL].pc);                     
    // TRACE_THREAD("SEM_DOWN: Second time context, PROC SYSCALL context for proc %d SR = %x, SSP=%lx, USP=%lx, PC=%lx", t->proc->pid, t->proc->ctxt[SYSCALL].sr, t->proc->ctxt[SYSCALL].ssp, t->proc->ctxt[SYSCALL].usp, t->proc->ctxt[SYSCALL].pc);
    /* ------------------------------------------------------------------
     * Wake-up path: sem_up has already:
     *   • removed us from the wait queue
     *   • cleared wait_type / sem_wait_obj
     *   • decremented sem->count on our behalf (token transfer)
     *   • called proc_thread_state_change(t, THREAD_STATE_READY) and
     *     add_to_ready_queue(t) — the scheduler then picked us up.
     *
     * All we need to do is ensure our state is RUNNING and return.
     * ------------------------------------------------------------------ */
 
    // proc_thread_schedule();

    if (semaphore_handle_signal_wakeup(t, sem) == EINTR) {
        return EINTR;
    }

    /* Clear any residual wait flags (defensive) */
    t->wait_type    &= ~WAIT_SEMAPHORE;
    t->sem_wait_obj  = NULL;
 
    if (t->state != THREAD_STATE_RUNNING)
        proc_thread_state_change(t, THREAD_STATE_RUNNING);
 
    TRACE_THREAD("SEM_DOWN: Thread %d woke up, sem=%p", t->tid, sem);
    return THREAD_SUCCESS;
}
 
 
/* =========================================================================
 * thread_semaphore_up  (sem_post equivalent)
 * =========================================================================
 *
 * Token-transfer semantics
 * ------------------------
 *   If waiters exist: give the token directly to the highest-priority
 *   waiter — do NOT increment count before waking, to avoid the race
 *   where a third thread could snatch the token before the waiter runs.
 *
 *   If no waiters: increment count unconditionally.
 *
 * Returns THREAD_SUCCESS (0) always (mirrors POSIX sem_post semantics).
 */
long thread_semaphore_up(struct semaphore *sem)
{
    struct thread  *current;
    struct thread  *waiter;
    struct thread  *prev_waiter;
    register unsigned short sr;
 
    if (!sem) {
        TRACE_THREAD("SEM_UP: NULL semaphore pointer");
        return EINVAL;
    }
 
    current = CURTHREAD;
    if (!current) {
        TRACE_THREAD("SEM_UP: No current thread");
        return EINVAL;
    }
 
    TRACE_THREAD("SEM_UP: sem=%p count=%ld wait_queue=%p",
                 sem, sem->count, sem->wait_queue);
 
    sr = splhigh();
 
    /* ------------------------------------------------------------------
     * Case 1: No waiters — simple increment.
     * ------------------------------------------------------------------ */
    if (!sem->wait_queue) {
        sem->count++;
        TRACE_THREAD("SEM_UP: No waiters, count now %ld", sem->count);
        spl(sr);
        return THREAD_SUCCESS;
    }
 
    /* ------------------------------------------------------------------
     * Case 2: Waiters present — find highest-priority one and wake it.
     *
     * We do NOT increment sem->count: the token is transferred directly.
     * This prevents a third thread from grabbing the token between the
     * sem_up and the moment the waiter actually runs.
     * ------------------------------------------------------------------ */
    prev_waiter = NULL;
    waiter      = find_highest_priority_thread_in_queue(sem->wait_queue,
                                                        &prev_waiter);
 
    if (!waiter) {
        /*
         * All entries in the queue were invalid (exited / bad magic).
         * Purge the stale queue, then increment the count normally.
         */
        TRACE_THREAD("SEM_UP: Wait queue had no valid waiters, purging");
        sem->wait_queue = NULL;
        sem->count++;
        spl(sr);
        return THREAD_SUCCESS;
    }

    /* Validate: signal may have woken the thread without removing it */
    if (!(waiter->wait_type & WAIT_SEMAPHORE) ||
            waiter->sem_wait_obj != sem) {
        TRACE_THREAD("SEM_UP: Skipping stale waiter %d (wait_type=0x%x, obj=%p)",
                        waiter->tid, waiter->wait_type, (void*)waiter->sem_wait_obj);
        /* Remove stale entry */
        if (prev_waiter)
            prev_waiter->next_wait = waiter->next_wait;
        else
            sem->wait_queue = waiter->next_wait;
        waiter->next_wait = NULL;
    }

    /* Remove waiter from wait queue */
    if (prev_waiter)
        prev_waiter->next_wait = waiter->next_wait;
    else
        sem->wait_queue = waiter->next_wait;
 
    waiter->next_wait   = NULL;
    waiter->wait_type  &= ~WAIT_SEMAPHORE;
    waiter->sem_wait_obj = NULL;
 
    /* Cancel any timeout */
    if (waiter->sleep_timeout) {
        TRACE_THREAD("SEM_UP: Canceling timeout for thread %d", waiter->tid);
        canceltimeout(waiter->sleep_timeout);
        waiter->sleep_timeout = NULL;
    }

    TRACE_THREAD("SEM_UP: Waking thread %d (priority %d)",
                 waiter->tid, waiter->priority);
 
    /* Clear any stale sleep state that may have been set concurrently */
    if (waiter->wakeup_time > 0) {
        waiter->wakeup_time = 0;
        remove_from_sleep_queue(waiter->proc, waiter);
    }
 
    /* Make waiter runnable */
    proc_thread_state_change(waiter, THREAD_STATE_READY);
    add_to_ready_queue(waiter);
 
    /*
     * Priority preemption: if the woken thread outranks us, yield
     * immediately so it can run without waiting for the next timer tick.
     *
     * Note: we release the interrupt lock BEFORE calling
     * proc_thread_schedule() because schedule() itself will call
     * splhigh/spl internally, and nesting the lock would deadlock on
     * any architecture that uses a non-reentrant spl scheme.
     */
    if (waiter->priority > current->priority) {
        TRACE_THREAD("SEM_UP: Waiter %d has higher priority, preempting",
                     waiter->tid);
        spl(sr);
        proc_thread_schedule();
        return THREAD_SUCCESS;
    }
 
    spl(sr);
    return THREAD_SUCCESS;
}