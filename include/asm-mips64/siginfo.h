/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1998, 1999, 2001 Ralf Baechle
 * Copyright (C) 2000, 2001 Silicon Graphics, Inc.
 */
#ifndef _ASM_SIGINFO_H
#define _ASM_SIGINFO_H

#define SIGEV_PAD_SIZE	((SIGEV_MAX_SIZE/sizeof(int)) - 4)

#define HAVE_ARCH_SIGINFO_T
#define HAVE_ARCH_SIGEVENT_T

#include <asm-generic/siginfo.h>

/* The sigval union matches IRIX 32/n32 ABIs for binary compatibility. */

/* This structure matches IRIX 32/n32 ABIs for binary compatibility but
   has Linux extensions.  */

typedef struct siginfo {
	int si_signo;
	int si_code;
	int si_errno;

	union {
		int _pad[SI_PAD_SIZE];

		/* kill() */
		struct {
			pid_t _pid;		/* sender's pid */
			uid_t _uid;		/* sender's uid */
		} _kill;

		/* SIGCHLD */
		struct {
			pid_t _pid;		/* which child */
			uid_t _uid;		/* sender's uid */
			clock_t _utime;
			int _status;		/* exit code */
			clock_t _stime;
		} _sigchld;

		/* IRIX SIGCHLD */
		struct {
			pid_t _pid;		/* which child */
			clock_t _utime;
			int _status;		/* exit code */
			clock_t _stime;
		} _irix_sigchld;

		/* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
		struct {
			void *_addr; /* faulting insn/memory ref. */
		} _sigfault;

		/* SIGPOLL, SIGXFSZ (To do ...)  */
		struct {
			long _band;	/* POLL_IN, POLL_OUT, POLL_MSG */
			int _fd;
		} _sigpoll;

		/* POSIX.1b timers */
		struct {
			unsigned int _timer1;
			unsigned int _timer2;
		} _timer;

		/* POSIX.1b signals */
		struct {
			pid_t _pid;		/* sender's pid */
			uid_t _uid;		/* sender's uid */
			sigval_t _sigval;
		} _rt;

	} _sifields;
} siginfo_t;

/*
 * si_code values
 * Again these have been choosen to be IRIX compatible.
 */
#undef SI_ASYNCIO
#undef SI_TIMER
#undef SI_MESGQ
#define SI_ASYNCIO	-2	/* sent by AIO completion */
#define SI_TIMER __SI_CODE(__SI_TIMER,-3) /* sent by timer expiration */
#define SI_MESGQ	-4	/* sent by real time mesq state change */

/*
 * sigevent definitions
 * 
 * It seems likely that SIGEV_THREAD will have to be handled from 
 * userspace, libpthread transmuting it to SIGEV_SIGNAL, which the
 * thread manager then catches and does the appropriate nonsense.
 * However, everything is written out here so as to not get lost.
 */
#undef SIGEV_NONE
#undef SIGEV_SIGNAL
#undef SIGEV_THREAD
#define SIGEV_NONE	128	/* other notification: meaningless */
#define SIGEV_SIGNAL	129	/* notify via signal */
#define SIGEV_CALLBACK	130	/* ??? */
#define SIGEV_THREAD	131	/* deliver via thread creation */

/* XXX This one isn't yet IRIX / ABI compatible.  */
typedef struct sigevent {
	int sigev_notify;
	sigval_t sigev_value;
	int sigev_signo;
	union {
		int _pad[SIGEV_PAD_SIZE];

		struct {
			void (*_function)(sigval_t);
			void *_attribute;	/* really pthread_attr_t */
		} _sigev_thread;
	} _sigev_un;
} sigevent_t;

#endif /* _ASM_SIGINFO_H */
