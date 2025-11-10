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
		 * a process
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

		/* various special things that should happen */
		if (act->sa_handler == SIG_IGN)
		{
			/* discard pending signals */
			p->sigpending &= ~(1L << sig);
		}

		/* I dunno if this is right, but bash seems to expect it */
		p->p_sigmask &= ~(1L << sig);
	}

	return E_OK;
}


/* Set extended handler */
long _cdecl
sys_p_sigaction_ext(short sig, void (*handler)(int, siginfo_t *, void *))
{
    PROC *p = get_curproc();
    
    if (sig < 1 || sig >= NSIG)
        return EBADARG;
    
    if (!p->p_sigacts)
        return EINVAL;
    
    p->p_sigacts->sa_sigaction_ext[sig] = handler;
    
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
	if (p->p_sigacts->thread_signals && t && t->tid > 0 && t->magic == CTXT_MAGIC) {
		/* Handle as thread-specific signal */
		void (*prev_handler)(int, void*) = t->sig_handlers[sig].handler;
		
		if (handler == SIG_DFL) {
			/* Clear thread-specific handler, fall back to process handler */
			TRACE_THREAD("Psignal: clearing thread-specific handler for thread %d sig %d", t->tid, sig);
			t->sig_handlers[sig].handler = NULL;
			t->sig_handlers[sig].arg = NULL;
			/* Discard pending signal for this thread */
			t->t_sigpending &= ~(1L << sig);
			/* Return previous handler, or SIG_DFL if there wasn't one */
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
		} else if (handler == SIG_IGN) {
			/* Set to ignore - clear thread handler and discard pending */
			TRACE_THREAD("Psignal: setting SIG_IGN for thread %d sig %d", t->tid, sig);
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
			t->sig_handlers[sig].handler = NULL;
			t->sig_handlers[sig].arg = NULL;
			/* Discard pending signals for this thread */
			t->t_sigpending &= ~(1L << sig);
			/* Also set at process level so behavior is consistent */
			goto set_process_handler;
		} else {
			/* Set custom handler for this thread */
			TRACE(("Psignal: setting custom handler %lx for thread %d sig %d", 
			       handler, t->tid, sig));
			ret = prev_handler ? (long)prev_handler : SIG_DFL;
			t->sig_handlers[sig].handler = (void(*)(int, void*))handler;
			t->sig_handlers[sig].arg = NULL;
		}
		
		/* Unmask the signal for this thread (per documented side effect) */
		t->t_sigmask &= ~(1L << sig);
		
		goto out;
	}

set_process_handler:
	/* Handle as process-level signal (original behavior) */
	/* This label allows SIG_IGN from threads to also set process-level handler */

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
	}
		
	/* Also discard from all threads if thread signals are enabled */
	if (p->p_sigacts->thread_signals) {
		struct thread *th;
		for (th = p->threads; th != NULL; th = th->next) {
			if (th->magic == CTXT_MAGIC) {
				th->t_sigpending &= ~(1L << sig);
			}
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
	ulong oldmask;

	TRACE (("Psigblock(%lx)",mask));

	/* some signals (e.g. SIGKILL) can't be masked */
	mask &= ~(UNMASKABLE);

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
	ulong oldmask;

	TRACE (("Psigsetmask(%lx)",mask));

	oldmask = p->p_sigmask;
	p->p_sigmask = mask & ~(UNMASKABLE);

	/* maybe we unmasked something */
	check_sigs ();

	return oldmask;
}

/*
 * p_sigpending: return which signals are pending delivery
 */
long _cdecl
sys_p_sigpending (void)
{
	PROC *p = get_curproc();

	TRACE (("Psigpending()"));

	/* clear out any that are going to be delivered soon */
	check_sigs ();

	/* note that signal #0 is used internally,
	 * so we don't tell the process about it
	 */
	return p->sigpending & ~1L;
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

	TRACE(("Psigpause(%lx)", mask));

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
static int 
dequeue_signal_info(PROC *p, struct thread *t, const sigset_t *set, siginfo_t *info)
{
	int sig = -1;
	struct sigqueue_entry *entry, *prev = NULL;
	unsigned short sr;
	
	if (!p || !set) 
		return -1;
	
	sr = splhigh();
	
	/* Check queued signals first (these have extended siginfo) */
	for (entry = p->sigqueue_head; entry; prev = entry, entry = entry->next) {
		if ((*set) & (1L << entry->info.si_signo)) {
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
	if (t && p->p_sigacts && p->p_sigacts->thread_signals) {
		ulong pending = t->t_sigpending & (*set);
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
		ulong pending = p->sigpending & (*set);
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
	
	TRACE(("Psigwaitinfo(%p, %p)", set, info));
	
	if (!set) {
		DEBUG(("Psigwaitinfo: null set"));
		return EINVAL;
	}
	
	/* Validate set doesn't include unmaskable signals */
	wait_set = *set & ~UNMASKABLE;
	if (!wait_set) {
		DEBUG(("Psigwaitinfo: empty set after removing unmaskable"));
		return EINVAL;
	}
	
	/* Check for already pending signals */
	sig = dequeue_signal_info(p, t, &wait_set, info);
	if (sig > 0) {
		TRACE(("Psigwaitinfo: returning immediate signal %d", sig));
		return sig;
	}
	
	/* Use existing thread signal wait mechanism if threads enabled */
	if (t && p->p_sigacts && p->p_sigacts->thread_signals) {
		TRACE(("Psigwaitinfo: using thread sigwait"));
		sig = proc_thread_signal_sigwait(wait_set, -1);
		if (sig > 0) {
			/* Signal received, fill siginfo */
			dequeue_signal_info(p, t, &wait_set, info);
		}
		return sig;
	}
	
	/* Fallback: process-level wait */
	TRACE(("Psigwaitinfo: process-level wait"));
	p->wait_cond = WAIT_SIGWAIT;
	while (1) {
		sig = dequeue_signal_info(p, NULL, &wait_set, info);
		if (sig > 0) {
			TRACE(("Psigwaitinfo: returning signal %d", sig));
			return sig;
		}
		
		/* Sleep waiting for signal */
		sleep(WAIT_Q, (long)&wait_set);
	}
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
	TIMEOUT *to = NULL;
	
	TRACE(("Psigtimedwait(%p, %p, %p)", set, info, timeout));
	
	if (!set) {
		DEBUG(("Psigtimedwait: null set"));
		return EINVAL;
	}
	
	/* Validate timeout */
	if (timeout) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || 
		    timeout->tv_nsec >= 1000000000L) {
			DEBUG(("Psigtimedwait: invalid timeout"));
			return EINVAL;
		}
		
		timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
		if (timeout_ms < 0) 
			timeout_ms = 0;
	}
	
	/* Validate set */
	wait_set = *set & ~UNMASKABLE;
	if (!wait_set) {
		DEBUG(("Psigtimedwait: empty set after removing unmaskable"));
		return EINVAL;
	}
	
	/* Check for already pending signals */
	sig = dequeue_signal_info(p, t, &wait_set, info);
	if (sig > 0) {
		TRACE(("Psigtimedwait: returning immediate signal %d", sig));
		return sig;
	}
	
	/* Use existing thread signal wait with timeout */
	if (t && p->p_sigacts && p->p_sigacts->thread_signals) {
		TRACE(("Psigtimedwait: using thread sigwait with timeout %ld ms", timeout_ms));
		sig = proc_thread_signal_sigwait(wait_set, timeout_ms);
		if (sig > 0) {
			/* Signal received, fill siginfo */
			dequeue_signal_info(p, t, &wait_set, info);
			return sig;
		}
		return (sig == 0) ? EAGAIN : sig;
	}
	
	/* Fallback: process-level wait with timeout */
	if (timeout_ms >= 0) {
		to = addtimeout(p, timeout_ms, (void _cdecl (*)(PROC *, long))wake);
		if (!to) {
			DEBUG(("Psigtimedwait: failed to add timeout"));
			return ENOMEM;
		}
		to->arg = (long)&wait_set;
	}
	
	TRACE(("Psigtimedwait: process-level wait with timeout"));
	p->wait_cond = WAIT_SIGWAIT;
	while (1) {
		sig = dequeue_signal_info(p, NULL, &wait_set, info);
		if (sig > 0) {
			if (to) 
				canceltimeout(to);
			TRACE(("Psigtimedwait: returning signal %d", sig));
			return sig;
		}
		
		sleep(WAIT_Q, (long)&wait_set);
		
		/* Check if woken by timeout */
		if (to && p->wait_cond != WAIT_SIGWAIT) {
			TRACE(("Psigtimedwait: timeout occurred"));
			return EAGAIN;
		}
	}
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
	
	/* Allocate queue entry */
	entry = kmalloc(sizeof(*entry));
	if (!entry) {
		DEBUG(("Psigqueue: out of memory"));
		return ENOMEM;
	}
	
	TRACE_THREAD("Psigqueue: allocated queue entry %p", entry);

	/* Fill siginfo */
	entry->info.si_signo = sig;
	entry->info.si_code = SI_QUEUE;
	entry->info.si_value = value;
	entry->info.si_pid = get_curproc()->pid;
	entry->info.si_uid = (unsigned short)get_curproc()->p_cred->ruid;
	entry->info.si_errno = 0;
	entry->info.si_addr = NULL;
	entry->info.si_status = 0;
	entry->info.si_band = 0;
	entry->queued = 1;
	entry->next = NULL;
	
	/* Add to queue atomically */
	sr = splhigh();
	
	if (p->sigqueue_tail)
		p->sigqueue_tail->next = entry;
	else
		p->sigqueue_head = entry;
	p->sigqueue_tail = entry;
	p->sigqueue_count++;
	
	/* Post signal to wake waiters */
	if (sig != 0) {
		TRACE_THREAD("Psigqueue: posting signal %d to pid %d", sig, pid);
		post_sig(p, sig);
	}
	
	spl(sr);
	
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