/*
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 *
 *
 * Copyright 1990,1991,1992,1994 Eric R. Smith.
 * All rights reserved.
 *
 */

/* dossig.c: dos signal handling routines */

# include "dossig.h"

# include "mint/arch/mfp.h"
# include "mint/asm.h"
# include "mint/credentials.h"
# include "mint/signal.h"

# include "proc.h"
# include "signal.h"
# include "util.h"

# include "proc_threads.h"
# include "proc_threads_signal.h"
/*
 * send a signal to another process. If pid > 0, send the signal just to
 * that process. If pid < 0, send the signal to all processes whose process
 * group is -pid. If pid == 0, send the signal to all processes with the
 * same process group id.
 *
 * note: post_sig just posts the signal to the process.
 */

long _cdecl
sys_p_kill (short pid, short sig)
{
	PROC *p;
	long r;

	TRACE (("Pkill(%d, %d)", pid, sig));
	
	if (sig < 0 || sig >= NSIG)
	{
		DEBUG (("Pkill: signal out of range"));
		return EBADARG;
	}

	if (sig == SIGSEGV || sig == SIGBUS)
	{
		PROC *curr = get_curproc();
		curr->exception_pc = curr->ctxt[SYSCALL].pc;
		curr->exception_addr = -1;
	}

	if (pid < 0)
		r = killgroup (-pid, sig, 0);
	else if (pid == 0)
		r = killgroup (get_curproc()->pgrp, sig, 0);
	else
	{
		p = pid2proc (pid);
		if (p == 0 || p->wait_q == ZOMBIE_Q || p->wait_q == TSR_Q)
		{
			DEBUG (("Pkill: pid %d not found", pid));
			return ENOENT;
		}

		if (get_curproc()->p_cred->ucr->euid && get_curproc()->p_cred->ruid != p->p_cred->ruid)
		{
			DEBUG (("Pkill: wrong user"));
			return EACCES;
		}

		/* if the user sends signal 0, don't deliver it -- for users,
		 * signal 0 is a null signal used to test the existence of
		 * a process. CRITICAL: sig=0 is NEVER delivered to post_sig()
		 */
		if (sig != 0)
			post_sig (p, sig);

		r = E_OK;
	}

	if (r == E_OK)
	{
		check_sigs ();
		TRACE (("Pkill: returning OK"));
	}

	return r;
}

/*
 * set a user-specified signal handler, POSIX.1 style
 * "oact", if non-null, gets the old signal handling
 * behaviour; "act", if non-null, specifies new
 * behaviour
 */

long _cdecl
sys_p_sigaction (short sig, const struct sigaction *act, struct sigaction *oact)
{
	PROC *p = get_curproc();
	struct thread *t = CURTHREAD;

	TRACE (("Psigaction(%d)", sig));

	assert (p->p_sigacts);

	if (sig < 1 || sig >= NSIG)
		return EBADARG;

	if (act && (sig == SIGKILL || sig == SIGSTOP))
		return EACCES;

	if (oact)
	{
		*oact = SIGACTION(p, sig);
		oact->sa_flags &= SAUSER;
	}

	if (act)
	{
		struct sigaction *sigact = & SIGACTION(p, sig);
		ushort flags;

		sigact->sa_handler = act->sa_handler;
		sigact->sa_mask = act->sa_mask & ~UNMASKABLE;

		/* only the flags in SAUSER can be changed by the user */
		flags = sigact->sa_flags & ~SAUSER;
		flags |= act->sa_flags & SAUSER;
		sigact->sa_flags = flags;
		if(p->current_thread) {
			TRACE_THREAD("Psigaction: New handler %p, mask 0x%lx, flags 0x%04x for signal %d", 
				sigact->sa_handler, sigact->sa_mask, sigact->sa_flags, sig);
		}
		/* various special things that should happen */
		if (act->sa_handler == SIG_IGN)
		{
			/* discard pending signals */
			if(p->current_thread) { TRACE_THREAD("Psigaction: Setting SIG_IGN for signal %d", sig); }
			p->sigpending &= ~(1L << sig);
			/* NEW: Also discard thread-specific pending signals */
			if (p->p_sigacts->thread_signals && t && t->magic == CTXT_MAGIC && t->tid > 0) {
				struct thread *th;
				for (th = p->threads; th != NULL; th = th->next) {
					if (th->magic == CTXT_MAGIC) {
						TRACE_THREAD("Psigaction: Clearing pending signal %d for thread %d due to SIG_IGN", sig, th->tid);
						CLEAR_THREAD_SIGPENDING(th, sig);
					}
				}
			}			
		}
		else if (act->sa_handler != SIG_DFL)
		{
			/*Handler installed - check for pending signals */
			if (p->p_sigacts->thread_signals && t && t->magic == CTXT_MAGIC && t->tid > 0) {
				/* Check if signal is pending for current thread */
				if (THREAD_SIGPENDING(t) & (1L << sig)) {
					TRACE_THREAD("Psigaction: signal %d pending for thread %d, will dispatch", 
					             sig, t->tid);
					/* Signal will be dispatched on next schedule or check_sigs */
				}
			}
		}		

		/* I dunno if this is right, but bash seems to expect it */
		p->p_sigmask &= ~(1L << sig);

		/* Also unmask for current thread if thread signals enabled */
		if (p->p_sigacts->thread_signals && t) {
			TRACE_THREAD("Psigaction: Unmasking signal %d for thread %d", sig, t->tid);
			t->t_sigmask &= ~(1L << sig);
		}
	}

	/* Dispatch any pending signals after sigaction completes */
	if (act && p->p_sigacts->thread_signals && t && t->tid > 0) {
		/* Immediate dispatch for signals that now have handlers */
		for (sig = 1; sig < NSIG; sig++) {
			if ((THREAD_SIGPENDING(t) & (1UL << sig)) && 
				t->sig_handlers[sig].handler) {
				TRACE_THREAD("SIGACTION: Immediately dispatching pending signal %d / calling handle_thread_signal()", sig);
				handle_thread_signal(t, sig);
			}
		}
	}

	return E_OK;
}

/* Set extended handler */
long _cdecl
sys_p_sigaction_ext(short sig, void (*handler)(int, siginfo_t *, void *))
{
    PROC *p = get_curproc();
    struct thread *t = CURTHREAD;

	TRACE_THREAD("SIGACTION: Psigaction_ext(%d, %p), pid %d, tid %d", sig, handler, p->pid, t ? t->tid : -1);

    if (sig < 1 || sig >= NSIG)
        return EBADARG;
    
    if (!p->p_sigacts)
        return EINVAL;
    
    p->p_sigacts->sa_sigaction_ext[sig] = handler;

    /* If thread signals enabled, unmask for current thread */
	if (p->p_sigacts->thread_signals && t && t->tid > 0) {
		TRACE_THREAD("Psigaction_ext: Unmasking signal %d for thread %d", sig, t->tid);
		t->t_sigmask &= ~(1L << sig);
		
		/* CRITICAL: Dispatch pending signals IMMEDIATELY after handler installation */
		if (handler && (THREAD_SIGPENDING(t) & (1L << sig))) {
			TRACE_THREAD("Psigaction_ext: signal %d pending, dispatching immediately / Calling handle_thread_signal()", sig);
			handle_thread_signal(t, sig);  // ← FIX: Direct call, bypass deferral
		}
	}

    /* NEW: Also unmask at process level */
    p->p_sigmask &= ~(1L << sig);
    
    /* NEW: Check process-level pending signals */
    if (handler && (p->sigpending & (1L << sig))) {
        check_sigs();
    }

    return E_OK;
}

/*
 * set a user-specified signal handler
 */
long _cdecl
sys_p_signal (short sig, long handler)
{
	PROC *p = get_curproc();
	struct thread *t = CURTHREAD;
	struct sigaction *sigact;
	long ret;

	TRACE (("Psignal(%u, %lx [%p])", sig, handler, p->p_sigacts));
	assert (p->p_sigacts && p->p_sigacts->links > 0);

	if (sig < 1 || sig >= NSIG)
	{
		ret = EBADARG;
		goto out;
	}

	if (sig == SIGKILL || sig == SIGSTOP)
	{
		ret = EACCES;
		goto out;
	}


	/* Check if we're in a thread context and thread signals are enabled */
	if (p->p_sigacts->thread_signals && t && t->tid >= 0 && t->magic == CTXT_MAGIC && t->tid > 0) {
		/* Handle as thread-specific signal */
		void (*prev_handler)(int, void*) = t->sig_handlers[sig].handler;
		
		if (handler == SIG_DFL) {
			/* Clear thread-specific handler, fall back to process handler */
			TRACE_THREAD("Psignal: clearing thread-specific handler for thread %d sig %d", t->tid, sig);
			t->sig_handlers[sig].handler = NULL;
			t->sig_handlers[sig].arg = NULL;
			/* Discard pending signal for this thread */
			CLEAR_THREAD_SIGPENDING(t, sig);
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
		} else if (handler == SIG_IGN) {
			/* Set to ignore - clear thread handler and discard pending */
			TRACE_THREAD("Psignal: setting SIG_IGN for thread %d sig %d", t->tid, sig);
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
			t->sig_handlers[sig].handler = NULL;
			t->sig_handlers[sig].arg = NULL;
			/* Discard pending signals for this thread */
			CLEAR_THREAD_SIGPENDING(t, sig);
			/* Also set at process level so behavior is consistent */
			goto set_process_handler;
		} else {
			/* Set custom handler for this thread */
			TRACE(("Psignal: setting custom handler %lx for thread %d sig %d", 
			       handler, t->tid, sig));
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
			t->sig_handlers[sig].handler = (void(*)(int, void*))handler;
			t->sig_handlers[sig].arg = NULL;

			/* CRITICAL: Dispatch pending signals IMMEDIATELY */
			if (THREAD_SIGPENDING(t) & (1L << sig)) {
				TRACE_THREAD("Psignal: signal %d pending, dispatching immediately / Calling handle_thread_signal()", sig);
				handle_thread_signal(t, sig);  // ← FIX: Direct call
			}	
		}
		
		/* Unmask the signal for this thread (per documented side effect) */
		t->t_sigmask &= ~(1L << sig);
		
		goto out;
	}

set_process_handler:
	/* Handle as process-level signal (original behavior) */
	sigact = & SIGACTION(p, sig);
	TRACE (("Psignal() sigact = %p", sigact));

	/* save old value for return */
	ret = sigact->sa_handler;

	sigact->sa_handler = handler;
	sigact->sa_mask = 0;
	sigact->sa_flags = 0;

	/* various special things that should happen */
	if (handler == SIG_IGN)
	{
		/* discard pending signals */
		p->sigpending &= ~(1L<<sig);
		/* Also discard from all threads if thread signals are enabled */
		if (p->p_sigacts->thread_signals && p->current_thread > 0 && p->current_thread->tid > 0) {
			struct thread *th;
			for (th = p->threads; th != NULL; th = th->next) {
				if (th->magic == CTXT_MAGIC) {
					CLEAR_THREAD_SIGPENDING(th, sig);
				}
			}
		}
	}
	else if (handler != SIG_DFL)
	{
		/* NEW: Handler installed at process level - dispatch pending signals */
		if (p->sigpending & (1L << sig)) {
			TRACE_THREAD("Psignal: signal %d pending at process level", sig);
			check_sigs();
		}
	}
	/* I dunno if this is right, but bash seems to expect it */
	p->p_sigmask &= ~(1L<<sig);

out:
	TRACE (("Psignal() ok (%li)", ret));
	return ret;
}

/*
 * block some signals. Returns the old signal mask.
 */
long _cdecl
sys_p_sigblock (ulong mask)
{
	PROC *p = get_curproc();
	struct thread *t = CURTHREAD;
	ulong oldmask;

	TRACE (("Psigblock(%lx)",mask));

	/* some signals (e.g. SIGKILL) can't be masked */
	mask &= ~(UNMASKABLE);

	/* NEW: If thread signals enabled, use thread mask */
	if (p->p_sigacts && p->p_sigacts->thread_signals && t && t->magic == CTXT_MAGIC && t->tid > 0) {
		oldmask = THREAD_SIGMASK(t);
		THREAD_SIGMASK_ADD(t, mask);
		return oldmask;
	}

	oldmask = p->p_sigmask;
	p->p_sigmask |= mask;

	return oldmask;
}

/*
 * set the signals that we're blocking. Some signals (e.g. SIGKILL)
 * can't be masked.
 * Returns the old mask.
 */
long _cdecl
sys_p_sigsetmask (ulong mask)
{
	PROC *p = get_curproc();
	struct thread *t = CURTHREAD;
	ulong oldmask;
	int sig;

	TRACE (("Psigsetmask(%lx)",mask));

	/* NEW: If thread signals enabled, use thread mask */
	if (p->p_sigacts && p->p_sigacts->thread_signals && t && t->magic == CTXT_MAGIC && t->tid > 0) {
		oldmask = THREAD_SIGMASK(t);
		THREAD_SIGMASK_SET(t, mask);
		
		/* CRITICAL: Dispatch any signals that are now unmasked AND have handlers */
		ulong unmasked_pending = THREAD_SIGPENDING(t) & ~THREAD_SIGMASK(t);
		if (unmasked_pending) {
			for (sig = 1; sig < NSIG; sig++) {
				if ((unmasked_pending & (1UL << sig)) && t->sig_handlers[sig].handler) {
					TRACE_THREAD("Psigsetmask: dispatching unmasked signal %d / Calling handle_thread_signal()", sig);
					handle_thread_signal(t, sig);
				}
			}
		}
		
		return oldmask;
	}

	oldmask = p->p_sigmask;
	p->p_sigmask = mask & ~(UNMASKABLE);

	/* maybe we unmasked something */
	check_sigs ();

	return oldmask;
}

/*
 * p_sigpending: return which signals are pending delivery
 */
long _cdecl sys_p_sigpending(void)
{
    PROC *p = get_curproc();
    struct thread *t = p->current_thread;
    ulong pending = p->sigpending;

    TRACE(("Psigpending()"));
    check_sigs();

    /* Include thread-specific pending signals for current thread */
    if (t && t->magic == CTXT_MAGIC && t->tid >= 0) {
        pending |= t->t_sigpending;
    }

    return pending & ~1UL;  // Mask out internal signal 0
}

/*
 * p_sigpause: atomically set the signals that we're blocking, then pause.
 * Some signals (e.g. SIGKILL) can't be masked.
 */
long _cdecl
sys_p_sigpause (ulong mask)
{
	PROC *p = get_curproc();
	ulong oldmask;
	struct thread *t = CURTHREAD;

	TRACE(("Psigpause(%lx)", mask));

	/* NEW: If thread signals enabled, use thread sigwait */
	if (p->p_sigacts && p->p_sigacts->thread_signals && t && t->magic == CTXT_MAGIC && t->tid > 0) {
		oldmask = THREAD_SIGMASK(t);
		THREAD_SIGMASK_SET(t, mask);
		
		/* Wait for any signal not in mask */
		proc_thread_signal_sigwait(~mask, -1);
		
		/* Restore old mask */
		THREAD_SIGMASK_SET(t, oldmask);
		
		/* maybe we unmasked something */
		TRACE_THREAD("Psigpause: checking for pending signals after sigwait / Calling dispatch_thread_signals()");
		dispatch_thread_signals(t);
		
		return E_OK;
	}

	oldmask = p->p_sigmask;
	p->p_sigmask = mask & ~(UNMASKABLE);

	if (p->sigpending & ~(p->p_sigmask))
		/* a signal is immediately pending */
		check_sigs ();
	else
		sleep (IO_Q, -1L);

	p->p_sigmask = oldmask;

	/* maybe we unmasked something */
	check_sigs();

	TRACE(("Psigpause: returning OK"));
	return E_OK;
}


/*
 * POSIX Real-Time Signal Functions
 */

/* Helper: check if signal is queued */
static int 
has_queued_signal(PROC *p, int sig)
{
	struct sigqueue_entry *entry;
	
	for (entry = p->sigqueue_head; entry; entry = entry->next) {
		if (entry->info.si_signo == sig)
			return 1;
	}
	return 0;
}

/* Helper: dequeue signal with info */
int 
dequeue_signal_info(PROC *p, struct thread *t, const sigset_t *set, siginfo_t *info)
{
	int sig = -1;
	struct sigqueue_entry *entry, *prev = NULL;
	unsigned short sr;
	
	if (!p || !set) 
		return -1;

	/* CRITICAL: Never dequeue signal 0 */
	sigset_t safe_set = (*set) & ~1UL;
		
	sr = splhigh();
	
	/* Check queued signals first (these have extended siginfo) */
	for (entry = p->sigqueue_head; entry; prev = entry, entry = entry->next) {
		if ((safe_set) & (1L << entry->info.si_signo)) {
			/* Found queued signal in set */
			if (info) {
				memcpy(info, &entry->info, sizeof(siginfo_t));
			}
			sig = entry->info.si_signo;
			
			/* Remove from queue */
			if (prev)
				prev->next = entry->next;
			else
				p->sigqueue_head = entry->next;
			
			if (entry == p->sigqueue_tail)
				p->sigqueue_tail = prev;
			
			p->sigqueue_count--;
			kfree(entry);
			
			/* Clear pending bit if no more queued instances */
			if (!has_queued_signal(p, sig))
				p->sigpending &= ~(1L << sig);
			
			spl(sr);
			return sig;
		}
	}
	
	/* Check thread-specific pending signals if threading enabled */
	if (t && p->p_sigacts && p->p_sigacts->thread_signals && t->magic == CTXT_MAGIC && t->tid > 0) {
		ulong pending = t->t_sigpending & safe_set;
		if (pending) {
			int i;
			for (i = 1; i < NSIG; i++) {
				if (pending & (1L << i)) {
					sig = i;
					t->t_sigpending &= ~(1L << i);
					if (info) {
						memset(info, 0, sizeof(siginfo_t));
						info->si_signo = sig;
						info->si_code = SI_USER;
						info->si_pid = 0;
						info->si_uid = 0;
					}
					spl(sr);
					return sig;
				}
			}
		}
	}
	
	/* Check process-level pending signals */
	{
		ulong pending = p->sigpending & safe_set;
		if (pending) {
			int i;
			for (i = 1; i < NSIG; i++) {
				if (pending & (1L << i)) {
					sig = i;
					p->sigpending &= ~(1L << i);
					if (info) {
						memset(info, 0, sizeof(siginfo_t));
						info->si_signo = sig;
						info->si_code = SI_USER;
						info->si_pid = 0;
						info->si_uid = 0;
					}
					spl(sr);
					return sig;
				}
			}
		}
	}
	
	spl(sr);
	return -1;
}

static void _cdecl
wake_condition (PROC *p, long arg)
{
	TRACE_THREAD("wake_condition: arg=%p", arg);
	wake(WAIT_Q, arg);
	p->wait_cond = 0;
}

/*
 * sigwaitinfo: wait for signals in set
 */
long _cdecl
sys_p_sigwaitinfo(const sigset_t *set, siginfo_t *info)
{
	PROC *p = get_curproc();
	struct thread *t = p->current_thread;
	int sig;
	sigset_t wait_set;
	sigset_t old_mask;
	
	TRACE(("Psigwaitinfo(%p, %p)", set, info));
	TRACE_THREAD("Psigwaitinfo: PID=%d, TID=%d, current_mask=0x%lx", 
	             p->pid, t ? t->tid : -1, p->p_sigmask);	
	
	if (!set) {
		DEBUG(("Psigwaitinfo: null set"));
		return EINVAL;
	}
	
	/* Validate set doesn't include unmaskable signals */
	/* CRITICAL: Remove unmaskable signals AND signal 0 */
	wait_set = *set & ~UNMASKABLE & ~1UL;
	
	if (!wait_set) {
		DEBUG(("Psigwaitinfo: empty set after removing unmaskable"));
		return EINVAL;
	}

	TRACE_THREAD("Psigwaitinfo: wait_set=0x%lx", wait_set);

	/* Block signals in wait_set to prevent handler delivery */
	old_mask = p->p_sigmask;
	p->p_sigmask &= ~wait_set;  /* Unmask signals we're waiting for */

	TRACE_THREAD("Psigwaitinfo: old_mask=0x%lx, new_mask=0x%lx, wait_set=0x%lx",
	             old_mask, p->p_sigmask, wait_set);

	/* Check for already pending signals */
	sig = dequeue_signal_info(p, t, &wait_set, info);
	if (sig > 0) {
		p->p_sigmask = old_mask;
		TRACE(("Psigwaitinfo: returning immediate signal %d", sig));
		TRACE_THREAD("Psigwaitinfo: Found immediate signal %d", sig);
		return sig;
	}
	
	/* Use existing thread signal wait mechanism if threads enabled */
	if (t && p->p_sigacts && p->p_sigacts->thread_signals && t->tid > 0) {
		TRACE_THREAD("Psigwaitinfo: using thread sigwait (thread_signals=1, tid=%d)", t->tid);
		/* Thread sigwait now handles dequeuing internally */
		sig = proc_thread_signal_sigwait(wait_set, -1);
		p->p_sigmask = old_mask;
		if (sig > 0 && info)
			dequeue_signal_info(p, t, &wait_set, info);
		return sig;
	}
	
	/* Fallback: process-level wait */
	TRACE_THREAD("Psigwaitinfo: process-level wait (thread_signals=%d, tid=%d)", 
	             p->p_sigacts ? p->p_sigacts->thread_signals : -1, t ? t->tid : -1);
	p->wait_cond = (long)&wait_set;
	TRACE_THREAD("Psigwaitinfo: Setting wait_cond=%p", (void*)p->wait_cond);
	while (1) {
		sig = dequeue_signal_info(p, NULL, &wait_set, info);
		if (sig > 0) {
			p->p_sigmask = old_mask;
			TRACE(("Psigwaitinfo: returning signal %d", sig));
			TRACE_THREAD("Psigwaitinfo: Found signal %d in loop", sig);
			p->p_flag &= ~P_FLAG_SIGWAIT;
			return sig;
		}

		/* Sleep waiting for signal */
		TRACE_THREAD("Psigwaitinfo: Going to sleep (wait_q=%d, wait_cond=%p, pid=%d)",
		             p->wait_q, (void*)p->wait_cond, p->pid);
		p->p_flag |= P_FLAG_SIGWAIT;		
		sleep(WAIT_Q, (long)&wait_set);
		p->p_flag &= ~P_FLAG_SIGWAIT;
		/* Check if we woke up for a different reason */
		if (p->wait_cond != (long)&wait_set) {
			TRACE_THREAD("Psigwaitinfo: Woke up with different wait_cond=%p", (void*)p->wait_cond);			
			break;
		}		
	}
	p->p_sigmask = old_mask;
	TRACE_THREAD("Psigwaitinfo: Returning EINTR");
	return EINTR;
}

/*
 * sigtimedwait: wait for signals with timeout
 */
long _cdecl
sys_p_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
	PROC *p = get_curproc();
	struct thread *t = p->current_thread;
	int sig;
	long timeout_ms = -1;
	sigset_t wait_set;
	sigset_t old_mask;
	TIMEOUT *to = NULL;
	
	TRACE_THREAD("Psigtimedwait(%p, %p, %p)", set, info, timeout);
	
	if (!set) {
		TRACE_THREAD("Psigtimedwait: null set");
		return EINVAL;
	}
	
	/* Validate timeout */
	if (timeout) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || 
		    timeout->tv_nsec >= 1000000000L) {
			TRACE_THREAD("Psigtimedwait: invalid timeout");
			return EINVAL;
		}
		TRACE_THREAD("Psigtimedwait: timeout=%ld.%ld", timeout->tv_sec, timeout->tv_nsec);
		timeout_ms = (timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000);
		TRACE_THREAD("Psigtimedwait: timeout_ms=%ld", timeout_ms);
		if (timeout_ms < 0) {
			TRACE_THREAD("Psigtimedwait: timeout overflow");
			timeout_ms = 0;
		}
	}
	
	/* 
	* Validate set 
	* Remove unmaskable signals AND signal 0 
	*/
	wait_set = *set & ~UNMASKABLE & ~1UL;

	if (!wait_set) {
		TRACE_THREAD("Psigtimedwait: empty set after removing unmaskable");
		return EINVAL;
	}
	TRACE_THREAD("Psigtimedwait: wait_set=%lx", wait_set);
	/* Unmask signals we're waiting for */
	old_mask = p->p_sigmask;
	p->p_sigmask &= ~wait_set;
	TRACE_THREAD("Psigtimedwait: p_sigmask=%lx", p->p_sigmask);
	/* Check for already pending signals */
	sig = dequeue_signal_info(p, t, &wait_set, info);
	if (sig > 0) {
		p->p_sigmask = old_mask;
		TRACE_THREAD("Psigtimedwait: returning immediate signal %d", sig);
		return sig;
	}
	
	/* Use existing thread signal wait with timeout */
	if (t && p->p_sigacts && p->p_sigacts->thread_signals && t->tid > 0) {
		TRACE_THREAD("Psigtimedwait: using thread sigwait with timeout %ld ms", timeout_ms);
		/* Thread sigwait now handles dequeuing internally */
		sig = proc_thread_signal_sigwait(wait_set, timeout_ms);
		p->p_sigmask = old_mask;
		TRACE_THREAD("Psigtimedwait: returning signal %d", sig);
		if (sig > 0 && info)
			dequeue_signal_info(p, t, &wait_set, info);
		return (sig == 0) ? EAGAIN : sig;
	}
	
	/* Fallback: process-level wait with timeout */
	if (timeout_ms >= 0) {
		to = addtimeout(p, timeout_ms, wake_condition);
		if (!to) {
			TRACE_THREAD("Psigtimedwait: failed to add timeout");
			return ENOMEM;
		}
		to->arg = (long)&wait_set;
	}
	
	TRACE_THREAD("Psigtimedwait: process-level wait with timeout");
	p->wait_cond = (long)&wait_set;

	while (1) {
		sig = dequeue_signal_info(p, NULL, &wait_set, info);
		if (sig > 0) {
			if (to) 
				canceltimeout(to);
			p->p_sigmask = old_mask;
			TRACE_THREAD("Psigtimedwait: returning signal %d", sig);
			return sig;
		}
		
		TRACE_THREAD("Psigtimedwait: going to sleep (wait_q=%d, wait_cond=%p, pid=%d)",
		             p->wait_q, (void*)p->wait_cond, p->pid);
		sleep(WAIT_Q, (long)&wait_set);
		
		/* Check if woken by timeout */
		if (to && p->wait_cond != (long)&wait_set) {
			TRACE_THREAD("Psigtimedwait: timeout occurred");
			p->p_sigmask = old_mask;
			return EAGAIN;
		}
				/* Check for spurious wakeup */
		if (p->wait_cond != (long)&wait_set) {
			TRACE_THREAD("Psigtimedwait: spurious wakeup");
			break;
		}
	}

	p->p_sigmask = old_mask;
	TRACE_THREAD("Psigtimedwait: Returning EINTR");
	return EINTR;
}

/*
 * sigqueue: send signal with data
 */
long _cdecl
sys_p_sigqueue(short pid, int sig, const union sigval value)
{
	PROC *p;
	struct sigqueue_entry *entry;
	unsigned short sr;
	siginfo_t info;
	
	TRACE(("Psigqueue(%d, %d, %ld/%p)", pid, sig, value.sival_int, value.sival_ptr));
	TRACE_THREAD("Psigqueue: from PID %d", get_curproc()->pid);
	TRACE_THREAD("Psigqueue: value = %ld/%p", value.sival_int, value.sival_ptr);

	if (sig < 1 || sig >= NSIG) {
		DEBUG(("Psigqueue: signal out of range"));
		TRACE_THREAD("Psigqueue: invalid signal %d", sig);
		return EINVAL;
	}
	
	/* Find target process */
	if (pid < 0) {
		TRACE_THREAD("Psigqueue: group signaling not supported, pid=%d", pid);
		DEBUG(("Psigqueue: group signaling not supported"));
		return ENOSYS;
	} else if (pid == 0) {
		p = get_curproc();
	} else {
		p = pid2proc(pid);
	}
	
	if (!p || p->wait_q == ZOMBIE_Q || p->wait_q == TSR_Q) {
		TRACE_THREAD("Psigqueue: pid %d not found", pid);
		DEBUG(("Psigqueue: pid %d not found", pid));
		return ENOENT;
	}
	
	/* Check permissions */
	if (get_curproc()->p_cred->ucr->euid && 
	    get_curproc()->p_cred->ruid != p->p_cred->ruid) {
		TRACE_THREAD("Psigqueue: wrong user, pid=%d, euid=%d, ruid=%d", pid, 
		             get_curproc()->p_cred->ucr->euid, get_curproc()->p_cred->ruid);
		DEBUG(("Psigqueue: wrong user"));
		return EACCES;
	}
	
	/* Check queue limit */
	if (p->sigqueue_count >= SIGQUEUE_MAX) {
		TRACE_THREAD("Psigqueue: queue full, count=%d", p->sigqueue_count);
		DEBUG(("Psigqueue: queue full"));
		return EAGAIN;
	}

	/* Build siginfo */
	memset(&info, 0, sizeof(info));
	info.si_signo = sig;
	info.si_code = SI_QUEUE;
	info.si_value.sival_int = value.sival_int;
	info.si_value.sival_ptr = value.sival_ptr;
	info.si_pid = get_curproc()->pid;
	info.si_uid = (unsigned short)get_curproc()->p_cred->ruid;
	
	/* NEW: Try thread-aware delivery first if threading enabled */
	if (p->p_sigacts && p->p_sigacts->thread_signals && IS_THREAD_USER_SIGNAL(sig) && p->current_thread->tid > 0) {
		struct thread *t;
		int delivered = 0;
		
		TRACE_THREAD("Psigqueue: attempting thread delivery for signal %d", sig);
		
		/* Try current thread first */
		if (p->current_thread && p->current_thread->tid > 0) {
			if (deliver_signal_to_thread(p, p->current_thread, sig, &info)) {
				delivered = 1;
				TRACE_THREAD("Psigqueue: delivered to current thread %d", 
				             p->current_thread->tid);
			}
		}
		
		/* Try threads waiting in sigwait */
		if (!delivered) {
			for (t = p->threads; t; t = t->next) {
				if (t->is_idle || t == p->current_thread || t->tid == 0) continue;
				
				if ((t->state & THREAD_STATE_BLOCKED) && 
				    (t->wait_type & WAIT_SIGNAL) && 
				    t->sig_wait_obj) {
					ulong wait_mask = (ulong)t->sig_wait_obj;
					if (wait_mask & (1UL << sig)) {
						if (deliver_signal_to_thread(p, t, sig, &info)) {
							delivered = 1;
							TRACE_THREAD("Psigqueue: delivered to waiting thread %d", t->tid);
							break;
						}
					}
				}
			}
		}
		
		if (delivered) {
			check_sigs();
			return E_OK;
		}
	}

	/* Allocate queue entry */
	entry = kmalloc(sizeof(*entry));
	if (!entry) {
		DEBUG(("Psigqueue: out of memory"));
		return ENOMEM;
	}
	
	TRACE_THREAD("Psigqueue: allocated queue entry %p", entry);

	/* Fill siginfo */
	memcpy(&entry->info, &info, sizeof(siginfo_t));
	entry->queued = 1;
	entry->next = NULL;

	TRACE_THREAD("Psigqueue: entry filled - si_signo=%ld, si_code=%ld, si_value=%ld/%p",
             entry->info.si_signo, entry->info.si_code,
             entry->info.si_value.sival_int, entry->info.si_value.sival_ptr);

	/* Add to queue atomically */
	sr = splhigh();
	
	if (p->sigqueue_tail)
		p->sigqueue_tail->next = entry;
	else
		p->sigqueue_head = entry;
	p->sigqueue_tail = entry;
	p->sigqueue_count++;

	spl(sr);

	/* Post signal - use thread-aware delivery if threading enabled */
	if (sig > 0 && sig < NSIG) {
		TRACE_THREAD("Psigqueue: posting signal %d to pid %d", sig, pid);
		/* If target process is multithreaded, use thread-aware delivery */
		if (p->p_sigacts && p->p_sigacts->thread_signals && p->current_thread->tid > 0) {
			TRACE_THREAD("Psigqueue: using thread-aware delivery");
			proc_thread_signal_aware_raise(p, sig);
		} else {
			TRACE_THREAD("Psigqueue: using post_sig()")
			post_sig(p, sig);
		}
	}
	
	TRACE_THREAD("Psigqueue: calling check_sigs()");
	check_sigs();
	
	TRACE(("Psigqueue: returning OK"));
	TRACE_THREAD("Psigqueue: signal %d queued to pid %d", sig, pid);
	return E_OK;
}

/* Cleanup function - call from process termination */
void 
cleanup_signal_queue(PROC *p)
{
	struct sigqueue_entry *entry, *next;
	unsigned short sr;
	
	if (!p) 
		return;
	
	sr = splhigh();
	
	entry = p->sigqueue_head;
	while (entry) {
		next = entry->next;
		kfree(entry);
		entry = next;
	}
	
	p->sigqueue_head = NULL;
	p->sigqueue_tail = NULL;
	p->sigqueue_count = 0;
	
	spl(sr);
}