/*
 * This file belongs to FreeMiNT. It's not in the original MiNT 1.12
 * distribution. See the file CHANGES for a detailed log of changes.
 *
 *
 * Copyright 2000 Frank Naumann <fnaumann@freemint.de>
 * All rights reserved.
 *
 * Please send suggestions, patches or bug reports to me or
 * the MiNT mailing list.
 *
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

# include "k_fork.h"

# include "libkern/libkern.h"

# include "mint/asm.h"
# include "mint/credentials.h"
# include "mint/signal.h"

# include "arch/mprot.h"

# include "filesys.h"
# include "k_prot.h"
# include "kmemory.h"
# include "memory.h"
# include "proc.h"
# include "proc_help.h"
# include "procfs.h"
# include "signal.h"
# include "time.h"
# include "util.h"

#include "proc_threads_debug.h"
#include "proc_threads_signal.h"
static int fork_setup_thread0(struct proc *parent, struct proc *child)
{
    struct thread *pt = parent->current_thread;
    struct thread *ct;
    
    /* Parent must be in thread0 */
    if (!pt || pt->tid != 0) {
        TRACE_THREAD("FORK: Can only fork from thread0");
        return ENOSYS;
    }
    
    /* Allocate child thread0 */
    ct = kmalloc(sizeof(struct thread));
    if (!ct) return ENOMEM;
    
    mint_bzero(ct, sizeof(*ct));
    
    /* Basic thread0 setup */
    ct->proc = child;
    ct->tid = 0;
    ct->magic = CTXT_MAGIC;
    ct->stack = child->stack;  /* Uses process kernel stack */
    ct->stack_top = (char*)child->stack + STKSIZE;
    ct->stack_size = STKSIZE;
    ct->stack_magic = STACK_MAGIC;
    
    /* Copy ONLY SYSCALL context - discard exception-frame SSP */
    memcpy(&ct->ctxt[SYSCALL], &pt->ctxt[SYSCALL], sizeof(CONTEXT));
    ct->ctxt[CURRENT] = ct->ctxt[SYSCALL];
    
    /* Use child's sysstack (already set to STKSIZE-12) for SSP */
    ct->ctxt[SYSCALL].ssp = child->sysstack;
    ct->ctxt[CURRENT].ssp = child->sysstack;
 
    /* Link to child process */
    child->current_thread = ct;
    child->threads = ct;
    child->num_threads = 1;
    child->total_threads = 1;
	child->p_flag &= ~P_FLAG_THREADED;

    /* Child returns 0 from fork() */
    ct->ctxt[SYSCALL].regs[0] = 0;
    ct->ctxt[CURRENT].regs[0] = 0;
    
    /* Ensure user mode */
    ct->ctxt[SYSCALL].sr &= ~0x2000;
    ct->ctxt[CURRENT].sr &= ~0x2000;
    
    /* Inherit signal mask */
    THREAD_SIGMASK_SET(ct, THREAD_SIGMASK(pt));

    TRACE_THREAD("FORK_CLONE: Inherited thread0 mask=0x%lx from parent, process mask=0x%lx",
                 THREAD_SIGMASK(ct), child->p_sigmask);
	
    /* Forked children start as traditional single-threaded processes
     * Clear ALL thread-specific signal state */
    ct->t_sigpending = 0;
    ct->t_sig_in_progress = 0;
    ct->sig_wait_obj = NULL;
    memset(ct->sig_handlers, 0, sizeof(ct->sig_handlers));
    
    if (child->p_sigacts) {
        /* Disable thread signals for forked child */
        child->p_sigacts->thread_signals = 0;
        child->p_sigacts->flags &= ~SAS_THREADED;
        TRACE_THREAD("FORK_CLONE: Disabled thread signals in child PID %d", child->pid);
    }

    TRACE_THREAD("FORK_CLONE: Child thread0 context set up:");
    TRACE_THREAD("  PC=%08lx, SR=%04x, USP=%08lx, SSP=%08lx (sysstack)",
                ct->ctxt[CURRENT].pc,
                ct->ctxt[CURRENT].sr,
                ct->ctxt[CURRENT].usp,
                ct->ctxt[CURRENT].ssp);
    TRACE_THREAD("  Child stack: base=%p, top=%p, sysstack=%lx", 
                child->stack, ct->stack_top, child->sysstack);

    return 0;
}

/*
 * duplicate process p1
 * return new process struct
 */
struct proc *
fork_proc1 (struct proc *p1, long flags, long *err)
{
	struct proc *p2;

	if(p1->current_thread && p1->current_thread->tid != 0){
		TRACE_THREAD("FORK: Current thread is not 0, it's %d for proc id %d, returning ENOSYS", p1->current_thread->tid, p1->pid);
		if(err) *err = ENOSYS;
		return NULL;
	}

	p2 = kmalloc (sizeof (*p2));
	if (!p2) goto nomem;

	/* copy */
	*p2 = *p1;

	p2->p_mem = NULL;
	p2->p_cred = NULL;
	p2->p_fd = NULL;
	p2->p_cwd = NULL;
	p2->p_sigacts = NULL;
	p2->p_limits = NULL;
	p2->p_ext = NULL;

	/* these things are not inherited
	 */
	p2->ppid = p1->pid;
	p2->pid = newpid ();
	p2->p_flag = 0;
	p2->sigpending = 0;
	p2->nsigs = 0;
	p2->sysstack = (long) (p2->stack + STKSIZE - 12);
	p2->ctxt[CURRENT].ssp = p2->sysstack;
	p2->ctxt[SYSCALL].ssp = (long) (p2->stack + ISTKSIZE);
	p2->stack_magic = STACK_MAGIC;
	p2->alarmtim = 0;
	p2->curpri = p2->pri;
	p2->slices = SLICES (p2->pri);

	p2->itimer[0].interval = 0;
	p2->itimer[0].reqtime = 0;
	p2->itimer[0].timeout = 0;
	p2->itimer[1].interval = 0;
	p2->itimer[1].reqtime = 0;
	p2->itimer[1].timeout = 0;
	p2->itimer[2].interval = 0;
	p2->itimer[2].reqtime = 0;
	p2->itimer[2].timeout = 0;

	((long *) p2->sysstack)[1] = FRAME_MAGIC;
	((long *) p2->sysstack)[2] = 0;
	((long *) p2->sysstack)[3] = 0;

	p2->usrtime = p2->systime = p2->chldstime = p2->chldutime = 0;

	/* child isn't traced */
	p2->ptracer = 0;
	p2->ptraceflags = 0;
	p2->ctxt[CURRENT].ptrace = 0;
	p2->ctxt[SYSCALL].ptrace = 0;

	p2->q_next = NULL;
	p2->wait_q = 0;

	/* Initialize thread-related fields */
	p2->current_thread = NULL;
	p2->threads = NULL;
	p2->num_threads = 0;
	p2->total_threads = 0;

	/* If parent is threaded, setup thread0 for child */
	if (p1->current_thread) {
		int result = fork_setup_thread0(p1, p2);
		if (result != 0) {
			if (err) *err = result;
			goto nomem;
		}
	}

	/* Initialize thread scheduling parameters - inherit from parent */
	p2->thread_preempt_interval = p1->thread_preempt_interval;
	p2->thread_default_timeslice = p1->thread_default_timeslice;
	p2->thread_min_timeslice = p1->thread_min_timeslice;
	p2->thread_rr_timeslice = p1->thread_rr_timeslice;

	/* Initialize thread queues */
	p2->sleep_queue = NULL;
	p2->ready_queue = NULL;
	p2->signal_wait_queue = NULL;
	p2->idle_thread = NULL;

	/* Thread timer initialization */
	p2->p_thread_timer.thread_id = 0;
	p2->p_thread_timer.enabled = 0;
	p2->p_thread_timer.timeout = NULL;
	p2->p_thread_timer.sr = 0;
	p2->p_thread_timer.in_handler = 0;

	/* Thread-specific data management */
	p2->thread_keys = NULL;
	p2->next_key = 0;
	p2->proc_tsd_data = NULL;
	// p2->p_sigacts->thread_signals = (p1->current_thread != NULL);

	/* Duplicate command line */
# ifndef M68000
	if (p2->real_cmdline != NULL \
		&& (*(long *)p2->real_cmdline))
# else
	if (p2->real_cmdline != NULL \
		&& (p2->real_cmdline [0] != 0 || p2->real_cmdline [1] != 0
			|| p2->real_cmdline [2] != 0 || p2->real_cmdline [3] != 0))
# endif
	{
		ulong *parent_cmdline = (ulong *) p2->real_cmdline;

		p2->real_cmdline = kmalloc ((*parent_cmdline) + 4);
		if (!p2->real_cmdline)
			goto nomem;

		memcpy (p2->real_cmdline, parent_cmdline, (*parent_cmdline) + 4);
	}
	else if (!(p1->p_flag & P_FLAG_SYS))
	{
		/* only warn if parent is not a system process
		 * XXX better initialize real_cmdline in proc.c???
		 */
		ALERT("Oops: no command line for %s (pid %d)", p2->fname, p2->pid);
		p2->real_cmdline = NULL;
	}


	if (flags & FORK_SHAREVM)
		p2->p_mem = share_mem (p1);
	else
		p2->p_mem = copy_mem (p1);

	p2->p_cred = kmalloc (sizeof (*p2->p_cred));
	if (p2->p_cred)
	{
		memcpy (p2->p_cred, p1->p_cred, sizeof (*p2->p_cred));
		p2->p_cred->links = 1;

		hold_cred (p2->p_cred->ucr);
	}

	if (flags & FORK_SHAREFILES)
		p2->p_fd = share_fd (p1);
	else
		p2->p_fd = copy_fd (p1);

	if (flags & FORK_SHARECWD)
		p2->p_cwd = share_cwd (p1);
	else
		p2->p_cwd = copy_cwd (p1);

	if (flags & FORK_SHARESIGS)
		p2->p_sigacts = share_sigacts (p1);
	else
		p2->p_sigacts = copy_sigacts (p1);

//	if (flags & FORK_SHARELIMITS)
//		p2->p_limits = share_limits (p1);
//	else
//		p2->p_limits = copy_limits (p1);

	if (flags & FORK_SHAREEXT)
		share_ext (p1, p2);
	else
		p2->p_ext = NULL; /* proc extensions can only be shared */


	/* checking memory allocation */
	if (!p2->p_mem || !p2->p_cred || !p2->p_fd || !p2->p_cwd || !p2->p_sigacts /*|| !p2->p_limits*/)
		goto nomem;


	/* Duplicate cookie for the executable file */
	dup_cookie (&p2->exe, &p1->exe);

	p2->started = xtime;

	/* notify proc extensions */
	proc_ext_on_fork(p1, flags, p2);

	/* now that memory ownership is copied, fill in page table
	 * WARNING: this must be done AFTER all memory allocations
	 *          (especially kmalloc)
	 */
	if (!(flags & FORK_SHAREVM))
	{
		init_page_table (p2, p2->p_mem);
		/* mapin the trampoline region */
		mark_proc_region (p2->p_mem, p2->p_mem->tp_reg, PROT_P, p2->pid);
	}

	/* hook into the process list */
	{
		unsigned short sr = splhigh ();

		p2->gl_next = proclist;
		proclist = p2;

		spl (sr);
	}

	return p2;

nomem:
	DEBUG (("fork_proc: insufficient memory"));

	if (p2)
	{
		if (p2->p_mem) free_mem (p2);
		if (p2->p_cred) { free_cred (p2->p_cred->ucr); kfree (p2->p_cred); }
		if (p2->p_fd) free_fd (p2);
		if (p2->p_cwd) free_cwd (p2);
		if (p2->p_sigacts) free_sigacts (p2);
		if (p2->p_limits) free_limits (p2);

		kfree (p2);
	}

	if (err) *err = ENOMEM;
	return NULL;
}

struct proc *
fork_proc (long flags, long *err)
{
	return fork_proc1 (get_curproc(), flags, err);
}

/*
 * p_vfork(): create a duplicate of  the current process. The parent's
 * address space is not saved. The parent is suspended until either the
 * child process (the duplicate) exits or does a Pexec which overlays its
 * memory space.
 */

long _cdecl
sys_pvfork (void)
{
	PROC *p;
	int new_pid;
	long p_sigmask;
	long r;

	p = fork_proc (FORK_SHAREEXT, &r); // (0, &r);
	if (!p)
	{
		DEBUG(("p_vfork: couldn't get new PROC struct"));
		return r;
	}

	assert (p->p_mem && p->p_mem->mem);

	/* set u:\proc time+date */
	procfs_stmp = xtime;

	p->ctxt[CURRENT] = p->ctxt[SYSCALL];
	p->ctxt[CURRENT].regs[0] = 0;		/* child returns a 0 from call */
	p->ctxt[CURRENT].sr &= ~(0x2000);	/* child must be in user mode */
# if 0	/* set up in fork_proc() */
	p->ctxt[CURRENT].ssp = (long)(p->stack + ISTKSIZE);
# endif

	/* watch out for job control signals, since our parent can never wake
	 * up to respond to them. solution: block them; exec_region (in mem.c)
	 * clears the signal mask, so an exec() will unblock them.
	 */
	p->p_sigmask |= (1L << SIGTSTP) | (1L << SIGTTIN) | (1L << SIGTTOU);

	TRACE(("p_vfork: parent going to sleep, wait_cond == %lx", (long)p));

	/* WARNING: This sleep() must absolutely not wake up until the child
	 * has released the memory space correctly. That's why we mask off
	 * all signals.
	 */
	p_sigmask = get_curproc()->p_sigmask;
	get_curproc()->p_sigmask = ~(((unsigned long) 1 << SIGKILL) | 1);

	{
		unsigned short sr = splhigh();

		add_q(READY_Q, p);	/* put it on the ready queue */
		sleep(WAIT_Q, (long)p);	/* while we wait for it */

		spl(sr);
	}

	TRACE(("p_vfork: parent waking up"));

	get_curproc()->p_sigmask = p_sigmask;
	/* note that the PROC structure pointed to by p may be freed during
	 * the check_sigs call!
	 */
	new_pid = p->pid;
	check_sigs ();	/* did we get any signals while sleeping? */
	return new_pid;
}

/*
 * p_fork(save): create a duplicate of  the current process. The parent's
 * address space is duplicated using a shadow region descriptor and a save
 * region, so both the parent and the child can be run. On a context switch
 * the parent's and child's address spaces will be exchanged (see proc.c).
 *
 * "txtsize" is the size of the process' TEXT area, if it has a valid one;
 * this is part of the second memory region attached (the basepage one)
 * and need not be saved (we assume processes don't write on their own
 * code segment).
 */

long _cdecl
sys_pfork (void)
{
	PROC *p;
	MEMREGION *m, *n;
	BASEPAGE *b;
	long txtsize;
	int i, j, new_pid;
	long r;

	p = fork_proc(FORK_SHAREEXT, &r); //fork_proc (0, &r);
	if (!p)
	{
		DEBUG (("p_fork: couldn't get new PROC struct"));
		return r;
	}

	assert (p->p_mem && p->p_mem->mem);

	/* set u:\proc time+date */
	procfs_stmp = xtime;

	/*
	 * save the parent's address space
	 */
	txtsize = p->p_mem->txtsize;

	TRACE (("p_fork: duplicating parent memory"));
	for (i = 0; i < p->p_mem->num_reg; i++)
	{
		m = p->p_mem->mem[i];
		if (m && !(m->mflags & M_SHARED))
		{
			if (m->mflags & M_SEEN)
			{
				/* Hmmm, this region has already been duplicated,
				 * reuse the duplicated region's descriptor and
				 * correct the link counts. Note that `m' still
				 * points to the parent's memory descriptor, so
				 * we must search the parent's memory table here.
				 */
				for (j = 0; get_curproc()->p_mem->mem[j] != m; j++)
					;
				n = p->p_mem->mem[j];
				n->links++;
				m->links--;
			}
			else
			{
				/* Okay we have to create a new shadow and save
				 * region for this one
				 *
				 * XXX assumes 1 is the MEMREGION of the
				 * textsegment; this SHOULD be reworked
				 */
				n = fork_region (m, i == 1 ? txtsize : 0);
				m->mflags |= M_SEEN; /* save links only once */
			}

			if (!n)
			{
				DEBUG(("p_fork: can't save parent's memory"));
				p->ppid = 0;		/* abandon the child */
				post_sig (p, SIGKILL);	/* then kill it */
				p->ctxt[CURRENT].pc = (long)check_sigs;
				p->ctxt[CURRENT].sr |= 0x2000; /* use supervisor stack */
# if 0	/* stack set up in fork_proc() */
				p->ctxt[CURRENT].ssp = (long)(p->stack + ISTKSIZE);
# endif
				p->pri = MAX_NICE+1;
				run_next(p, 1);
				yield();
				return ENOMEM;
			}
			p->p_mem->mem[i] = n;
		}
	}

	assert (get_curproc()->p_mem && get_curproc()->p_mem->mem);

	/* The save regions are initially attached to the child's regions. To
	 * prevent swapping of the region and its save region (which should
	 * still be identical) on the next context switch, we attach them here
	 * to the current process.
	 */
	for (i = 0; i < get_curproc()->p_mem->num_reg; i++)
	{
		m = get_curproc()->p_mem->mem[i];
		if (m)
		{
			m->mflags &= ~M_SEEN;
			n = p->p_mem->mem[i];
			if (n->save)
			{
				m->save = n->save;
				n->save = 0;
			}
		}
	}

	/* Correct the parent basepage pointer on the child, it stills
	 * points to its grandparent's basepage.
	 */
	b = (BASEPAGE *) p->p_mem->mem[1]->loc;
	b->p_parent = (BASEPAGE *) get_curproc()->p_mem->mem[1]->loc;

	p->ctxt[CURRENT] = p->ctxt[SYSCALL];
	p->ctxt[CURRENT].regs[0] = 0;		/* child returns a 0 from call */
	p->ctxt[CURRENT].sr &= ~(0x2000);	/* child must be in user mode */
# if 0	/* set up in fork_proc() */
	p->ctxt[CURRENT].ssp = (long)(p->stack + ISTKSIZE);
# endif

	/* Now run the child process. We want it to run ASAP, so we
	 * temporarily give it high priority and put it first on the
	 * run queue. Warning: After the yield() the "p" structure may
	 * not exist any more.
	 */
	run_next(p, 3);
	new_pid = p->pid;
	yield();

	return new_pid;
}
