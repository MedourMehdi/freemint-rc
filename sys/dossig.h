/*
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 */

# ifndef _dossig_h
# define _dossig_h

# include "mint/mint.h"
# include "mint/signal.h"
# include "mint/time.h"

long _cdecl sys_p_kill (short pid, short sig);
long _cdecl sys_p_sigaction (short sig, const struct sigaction *act, struct sigaction *oact);
long _cdecl sys_p_signal (short sig, long handler);
long _cdecl sys_p_sigblock (ulong mask);
long _cdecl sys_p_sigsetmask (ulong mask);
long _cdecl sys_p_sigpending (void);
long _cdecl sys_p_sigpause (ulong mask);


/* POSIX real-time signal functions */
long _cdecl sys_p_sigaction_ext(short sig, void (*handler)(int, siginfo_t *, void *));
long _cdecl sys_p_sigwaitinfo(const sigset_t *set, siginfo_t *info);
long _cdecl sys_p_sigtimedwait(const sigset_t *set, siginfo_t *info, 
                               const struct timespec *timeout);
long _cdecl sys_p_sigqueue(short pid, int sig, const union sigval value);

void cleanup_signal_queue(PROC *p);

# endif /* _dossig_h */
