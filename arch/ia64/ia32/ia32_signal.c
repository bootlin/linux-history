/*
 * IA32 Architecture-specific signal handling support.
 *
 * Copyright (C) 1999, 2001-2002 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 * Copyright (C) 1999 Arun Sharma <arun.sharma@intel.com>
 * Copyright (C) 2000 VA Linux Co
 * Copyright (C) 2000 Don Dugger <n0ano@valinux.com>
 *
 * Derived from i386 and Alpha versions.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/personality.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/wait.h>
#include <linux/compat.h>

#include <asm/intrinsics.h>
#include <asm/uaccess.h>
#include <asm/rse.h>
#include <asm/sigcontext.h>
#include <asm/segment.h>

#include "ia32priv.h"

#include "../kernel/sigframe.h"

#define A(__x)		((unsigned long)(__x))

#define DEBUG_SIG	0
#define _BLOCKABLE	(~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

#define __IA32_NR_sigreturn            119
#define __IA32_NR_rt_sigreturn         173

#ifdef ASM_SUPPORTED
/*
 * Don't let GCC uses f16-f31 so that save_ia32_fpstate_live() and
 * restore_ia32_fpstate_live() can be sure the live register contain user-level state.
 */
register double f16 asm ("f16"); register double f17 asm ("f17");
register double f18 asm ("f18"); register double f19 asm ("f19");
register double f20 asm ("f20"); register double f21 asm ("f21");
register double f22 asm ("f22"); register double f23 asm ("f23");

register double f24 asm ("f24"); register double f25 asm ("f25");
register double f26 asm ("f26"); register double f27 asm ("f27");
register double f28 asm ("f28"); register double f29 asm ("f29");
register double f30 asm ("f30"); register double f31 asm ("f31");
#endif

struct sigframe_ia32
{
       int pretcode;
       int sig;
       struct sigcontext_ia32 sc;
       struct _fpstate_ia32 fpstate;
       unsigned int extramask[_COMPAT_NSIG_WORDS-1];
       char retcode[8];
};

struct rt_sigframe_ia32
{
       int pretcode;
       int sig;
       int pinfo;
       int puc;
       siginfo_t32 info;
       struct ucontext_ia32 uc;
       struct _fpstate_ia32 fpstate;
       char retcode[8];
};

int
copy_siginfo_from_user32 (siginfo_t *to, siginfo_t32 *from)
{
	unsigned long tmp;
	int err;

	if (!access_ok(VERIFY_READ, from, sizeof(siginfo_t32)))
		return -EFAULT;

	err = __get_user(to->si_signo, &from->si_signo);
	err |= __get_user(to->si_errno, &from->si_errno);
	err |= __get_user(to->si_code, &from->si_code);

	if (from->si_code < 0)
		err |= __copy_from_user(&to->_sifields._pad, &from->_sifields._pad, SI_PAD_SIZE);
	else {
		switch (from->si_code >> 16) {
		      case __SI_CHLD >> 16:
			err |= __get_user(to->si_utime, &from->si_utime);
			err |= __get_user(to->si_stime, &from->si_stime);
			err |= __get_user(to->si_status, &from->si_status);
		      default:
			err |= __get_user(to->si_pid, &from->si_pid);
			err |= __get_user(to->si_uid, &from->si_uid);
			break;
		      case __SI_FAULT >> 16:
			err |= __get_user(tmp, &from->si_addr);
			to->si_addr = (void *) tmp;
			break;
		      case __SI_POLL >> 16:
			err |= __get_user(to->si_band, &from->si_band);
			err |= __get_user(to->si_fd, &from->si_fd);
			break;
		      case __SI_RT: /* This is not generated by the kernel as of now.  */
		      case __SI_MESGQ:
			err |= __get_user(to->si_pid, &from->si_pid);
			err |= __get_user(to->si_uid, &from->si_uid);
			err |= __get_user(to->si_int, &from->si_int);
			break;
		}
	}
	return err;
}

int
copy_siginfo_to_user32 (siginfo_t32 *to, siginfo_t *from)
{
	unsigned int addr;
	int err;

	if (!access_ok(VERIFY_WRITE, to, sizeof(siginfo_t32)))
		return -EFAULT;

	/* If you change siginfo_t structure, please be sure
	   this code is fixed accordingly.
	   It should never copy any pad contained in the structure
	   to avoid security leaks, but must copy the generic
	   3 ints plus the relevant union member.
	   This routine must convert siginfo from 64bit to 32bit as well
	   at the same time.  */
	err = __put_user(from->si_signo, &to->si_signo);
	err |= __put_user(from->si_errno, &to->si_errno);
	err |= __put_user((short)from->si_code, &to->si_code);
	if (from->si_code < 0)
		err |= __copy_to_user(&to->_sifields._pad, &from->_sifields._pad, SI_PAD_SIZE);
	else {
		switch (from->si_code >> 16) {
		case __SI_CHLD >> 16:
			err |= __put_user(from->si_utime, &to->si_utime);
			err |= __put_user(from->si_stime, &to->si_stime);
			err |= __put_user(from->si_status, &to->si_status);
		default:
			err |= __put_user(from->si_pid, &to->si_pid);
			err |= __put_user(from->si_uid, &to->si_uid);
			break;
		case __SI_FAULT >> 16:
			err |= __put_user((long)from->si_addr, &to->si_addr);
			break;
		case __SI_POLL >> 16:
			err |= __put_user(from->si_band, &to->si_band);
			err |= __put_user(from->si_fd, &to->si_fd);
			break;
		case __SI_TIMER >> 16:
			err |= __put_user(from->si_tid, &to->si_tid);
			err |= __put_user(from->si_overrun, &to->si_overrun);
			addr = (unsigned long) from->si_ptr;
			err |= __put_user(addr, &to->si_ptr);
			break;
		/* case __SI_RT: This is not generated by the kernel as of now.  */
		}
	}
	return err;
}


/*
 *  SAVE and RESTORE of ia32 fpstate info, from ia64 current state
 *  Used in exception handler to pass the fpstate to the user, and restore
 *  the fpstate while returning from the exception handler.
 *
 *    fpstate info and their mapping to IA64 regs:
 *    fpstate    REG(BITS)      Attribute    Comments
 *    cw         ar.fcr(0:12)                with bits 7 and 6 not used
 *    sw         ar.fsr(0:15)
 *    tag        ar.fsr(16:31)               with odd numbered bits not used
 *                                           (read returns 0, writes ignored)
 *    ipoff      ar.fir(0:31)
 *    cssel      ar.fir(32:47)
 *    dataoff    ar.fdr(0:31)
 *    datasel    ar.fdr(32:47)
 *
 *    _st[(0+TOS)%8]   f8
 *    _st[(1+TOS)%8]   f9
 *    _st[(2+TOS)%8]   f10
 *    _st[(3+TOS)%8]   f11                   (f8..f11 from ptregs)
 *      : :            :                     (f12..f15 from live reg)
 *      : :            :
 *    _st[(7+TOS)%8]   f15                   TOS=sw.top(bits11:13)
 *
 *    status     Same as sw     RO
 *    magic      0                           as X86_FXSR_MAGIC in ia32
 *    mxcsr      Bits(7:15)=ar.fcr(39:47)
 *               Bits(0:5) =ar.fsr(32:37)    with bit 6 reserved
 *    _xmm[0..7] f16..f31                    (live registers)
 *                                           with _xmm[0]
 *                                             Bit(64:127)=f17(0:63)
 *                                             Bit(0:63)=f16(0:63)
 *    All other fields unused...
 */

static int
save_ia32_fpstate_live (struct _fpstate_ia32 *save)
{
	struct task_struct *tsk = current;
	struct pt_regs *ptp;
	struct _fpreg_ia32 *fpregp;
	char buf[32];
	unsigned long fsr, fcr, fir, fdr;
	unsigned long new_fsr;
	unsigned long num128[2];
	unsigned long mxcsr=0;
	int fp_tos, fr8_st_map;

	if (!access_ok(VERIFY_WRITE, save, sizeof(*save)))
		return -EFAULT;

	/* Read in fsr, fcr, fir, fdr and copy onto fpstate */
	fsr = ia64_getreg(_IA64_REG_AR_FSR);
	fcr = ia64_getreg(_IA64_REG_AR_FCR);
	fir = ia64_getreg(_IA64_REG_AR_FIR);
	fdr = ia64_getreg(_IA64_REG_AR_FDR);

	/*
	 * We need to clear the exception state before calling the signal handler. Clear
	 * the bits 15, bits 0-7 in fp status word. Similar to the functionality of fnclex
	 * instruction.
	 */
	new_fsr = fsr & ~0x80ff;
	ia64_setreg(_IA64_REG_AR_FSR, new_fsr);

	__put_user(fcr & 0xffff, &save->cw);
	__put_user(fsr & 0xffff, &save->sw);
	__put_user((fsr>>16) & 0xffff, &save->tag);
	__put_user(fir, &save->ipoff);
	__put_user((fir>>32) & 0xffff, &save->cssel);
	__put_user(fdr, &save->dataoff);
	__put_user((fdr>>32) & 0xffff, &save->datasel);
	__put_user(fsr & 0xffff, &save->status);

	mxcsr = ((fcr>>32) & 0xff80) | ((fsr>>32) & 0x3f);
	__put_user(mxcsr & 0xffff, &save->mxcsr);
	__put_user( 0, &save->magic); //#define X86_FXSR_MAGIC   0x0000

	/*
	 * save f8..f11  from pt_regs
	 * save f12..f15 from live register set
	 */
	/*
	 *  Find the location where f8 has to go in fp reg stack.  This depends on
	 *  TOP(11:13) field of sw. Other f reg continue sequentially from where f8 maps
	 *  to.
	 */
	fp_tos = (fsr>>11)&0x7;
	fr8_st_map = (8-fp_tos)&0x7;
	ptp = ia64_task_regs(tsk);
	fpregp = (struct _fpreg_ia32 *)(((unsigned long)buf + 15) & ~15);
	ia64f2ia32f(fpregp, &ptp->f8);
	copy_to_user(&save->_st[(0+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64f2ia32f(fpregp, &ptp->f9);
	copy_to_user(&save->_st[(1+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64f2ia32f(fpregp, &ptp->f10);
	copy_to_user(&save->_st[(2+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64f2ia32f(fpregp, &ptp->f11);
	copy_to_user(&save->_st[(3+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));

	ia64_stfe(fpregp, 12);
	copy_to_user(&save->_st[(4+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64_stfe(fpregp, 13);
	copy_to_user(&save->_st[(5+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64_stfe(fpregp, 14);
	copy_to_user(&save->_st[(6+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));
	ia64_stfe(fpregp, 15);
	copy_to_user(&save->_st[(7+fr8_st_map)&0x7], fpregp, sizeof(struct _fpreg_ia32));

	ia64_stf8(&num128[0], 16);
	ia64_stf8(&num128[1], 17);
	copy_to_user(&save->_xmm[0], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 18);
	ia64_stf8(&num128[1], 19);
	copy_to_user(&save->_xmm[1], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 20);
	ia64_stf8(&num128[1], 21);
	copy_to_user(&save->_xmm[2], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 22);
	ia64_stf8(&num128[1], 23);
	copy_to_user(&save->_xmm[3], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 24);
	ia64_stf8(&num128[1], 25);
	copy_to_user(&save->_xmm[4], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 26);
	ia64_stf8(&num128[1], 27);
	copy_to_user(&save->_xmm[5], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 28);
	ia64_stf8(&num128[1], 29);
	copy_to_user(&save->_xmm[6], num128, sizeof(struct _xmmreg_ia32));

	ia64_stf8(&num128[0], 30);
	ia64_stf8(&num128[1], 31);
	copy_to_user(&save->_xmm[7], num128, sizeof(struct _xmmreg_ia32));
	return 0;
}

static int
restore_ia32_fpstate_live (struct _fpstate_ia32 *save)
{
	struct task_struct *tsk = current;
	struct pt_regs *ptp;
	unsigned int lo, hi;
	unsigned long num128[2];
	unsigned long num64, mxcsr;
	struct _fpreg_ia32 *fpregp;
	char buf[32];
	unsigned long fsr, fcr, fir, fdr;
	int fp_tos, fr8_st_map;

	if (!access_ok(VERIFY_READ, save, sizeof(*save)))
		return(-EFAULT);

	/*
	 * Updating fsr, fcr, fir, fdr.
	 * Just a bit more complicated than save.
	 * - Need to make sure that we don't write any value other than the
	 *   specific fpstate info
	 * - Need to make sure that the untouched part of frs, fdr, fir, fcr
	 *   should remain same while writing.
	 * So, we do a read, change specific fields and write.
	 */
	fsr = ia64_getreg(_IA64_REG_AR_FSR);
	fcr = ia64_getreg(_IA64_REG_AR_FCR);
	fir = ia64_getreg(_IA64_REG_AR_FIR);
	fdr = ia64_getreg(_IA64_REG_AR_FDR);

	__get_user(mxcsr, (unsigned int *)&save->mxcsr);
	/* setting bits 0..5 8..12 with cw and 39..47 from mxcsr */
	__get_user(lo, (unsigned int *)&save->cw);
	num64 = mxcsr & 0xff10;
	num64 = (num64 << 32) | (lo & 0x1f3f);
	fcr = (fcr & (~0xff1000001f3f)) | num64;

	/* setting bits 0..31 with sw and tag and 32..37 from mxcsr */
	__get_user(lo, (unsigned int *)&save->sw);
	/* set bits 15,7 (fsw.b, fsw.es) to reflect the current error status */
	if ( !(lo & 0x7f) )
		lo &= (~0x8080);
	__get_user(hi, (unsigned int *)&save->tag);
	num64 = mxcsr & 0x3f;
	num64 = (num64 << 16) | (hi & 0xffff);
	num64 = (num64 << 16) | (lo & 0xffff);
	fsr = (fsr & (~0x3fffffffff)) | num64;

	/* setting bits 0..47 with cssel and ipoff */
	__get_user(lo, (unsigned int *)&save->ipoff);
	__get_user(hi, (unsigned int *)&save->cssel);
	num64 = hi & 0xffff;
	num64 = (num64 << 32) | lo;
	fir = (fir & (~0xffffffffffff)) | num64;

	/* setting bits 0..47 with datasel and dataoff */
	__get_user(lo, (unsigned int *)&save->dataoff);
	__get_user(hi, (unsigned int *)&save->datasel);
	num64 = hi & 0xffff;
	num64 = (num64 << 32) | lo;
	fdr = (fdr & (~0xffffffffffff)) | num64;

	ia64_setreg(_IA64_REG_AR_FSR, fsr);
	ia64_setreg(_IA64_REG_AR_FCR, fcr);
	ia64_setreg(_IA64_REG_AR_FIR, fir);
	ia64_setreg(_IA64_REG_AR_FDR, fdr);

	/*
	 * restore f8..f11 onto pt_regs
	 * restore f12..f15 onto live registers
	 */
	/*
	 *  Find the location where f8 has to go in fp reg stack.  This depends on
	 *  TOP(11:13) field of sw. Other f reg continue sequentially from where f8 maps
	 *  to.
	 */
	fp_tos = (fsr>>11)&0x7;
	fr8_st_map = (8-fp_tos)&0x7;
	fpregp = (struct _fpreg_ia32 *)(((unsigned long)buf + 15) & ~15);

	ptp = ia64_task_regs(tsk);
	copy_from_user(fpregp, &save->_st[(0+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia32f2ia64f(&ptp->f8, fpregp);
	copy_from_user(fpregp, &save->_st[(1+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia32f2ia64f(&ptp->f9, fpregp);
	copy_from_user(fpregp, &save->_st[(2+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia32f2ia64f(&ptp->f10, fpregp);
	copy_from_user(fpregp, &save->_st[(3+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia32f2ia64f(&ptp->f11, fpregp);

	copy_from_user(fpregp, &save->_st[(4+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia64_ldfe(12, fpregp);
	copy_from_user(fpregp, &save->_st[(5+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia64_ldfe(13, fpregp);
	copy_from_user(fpregp, &save->_st[(6+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia64_ldfe(14, fpregp);
	copy_from_user(fpregp, &save->_st[(7+fr8_st_map)&0x7], sizeof(struct _fpreg_ia32));
	ia64_ldfe(15, fpregp);

	copy_from_user(num128, &save->_xmm[0], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(16, &num128[0]);
	ia64_ldf8(17, &num128[1]);

	copy_from_user(num128, &save->_xmm[1], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(18, &num128[0]);
	ia64_ldf8(19, &num128[1]);

	copy_from_user(num128, &save->_xmm[2], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(20, &num128[0]);
	ia64_ldf8(21, &num128[1]);

	copy_from_user(num128, &save->_xmm[3], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(22, &num128[0]);
	ia64_ldf8(23, &num128[1]);

	copy_from_user(num128, &save->_xmm[4], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(24, &num128[0]);
	ia64_ldf8(25, &num128[1]);

	copy_from_user(num128, &save->_xmm[5], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(26, &num128[0]);
	ia64_ldf8(27, &num128[1]);

	copy_from_user(num128, &save->_xmm[6], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(28, &num128[0]);
	ia64_ldf8(29, &num128[1]);

	copy_from_user(num128, &save->_xmm[7], sizeof(struct _xmmreg_ia32));
	ia64_ldf8(30, &num128[0]);
	ia64_ldf8(31, &num128[1]);
	return 0;
}

static inline void
sigact_set_handler (struct k_sigaction *sa, unsigned int handler, unsigned int restorer)
{
	if (handler + 1 <= 2)
		/* SIG_DFL, SIG_IGN, or SIG_ERR: must sign-extend to 64-bits */
		sa->sa.sa_handler = (__sighandler_t) A((int) handler);
	else
		sa->sa.sa_handler = (__sighandler_t) (((unsigned long) restorer << 32) | handler);
}

asmlinkage long
ia32_rt_sigsuspend (compat_sigset_t *uset, unsigned int sigsetsize, struct sigscratch *scr)
{
	extern long ia64_do_signal (sigset_t *oldset, struct sigscratch *scr, long in_syscall);
	sigset_t oldset, set;

	scr->scratch_unat = 0;	/* avoid leaking kernel bits to user level */
	memset(&set, 0, sizeof(&set));

	if (sigsetsize > sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&set.sig, &uset->sig, sigsetsize))
		return -EFAULT;

	sigdelsetmask(&set, ~_BLOCKABLE);

	spin_lock_irq(&current->sighand->siglock);
	{
		oldset = current->blocked;
		current->blocked = set;
		recalc_sigpending();
	}
	spin_unlock_irq(&current->sighand->siglock);

	/*
	 * The return below usually returns to the signal handler.  We need to pre-set the
	 * correct error code here to ensure that the right values get saved in sigcontext
	 * by ia64_do_signal.
	 */
	scr->pt.r8 = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (ia64_do_signal(&oldset, scr, 1))
			return -EINTR;
	}
}

asmlinkage long
ia32_sigsuspend (unsigned int mask, struct sigscratch *scr)
{
	return ia32_rt_sigsuspend((compat_sigset_t *)&mask, sizeof(mask), scr);
}

asmlinkage long
sys32_signal (int sig, unsigned int handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	sigact_set_handler(&new_sa, handler, 0);
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : IA32_SA_HANDLER(&old_sa);
}

asmlinkage long
sys32_rt_sigaction (int sig, struct sigaction32 *act,
		    struct sigaction32 *oact, unsigned int sigsetsize)
{
	struct k_sigaction new_ka, old_ka;
	unsigned int handler, restorer;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(compat_sigset_t))
		return -EINVAL;

	if (act) {
		ret = get_user(handler, &act->sa_handler);
		ret |= get_user(new_ka.sa.sa_flags, &act->sa_flags);
		ret |= get_user(restorer, &act->sa_restorer);
		ret |= copy_from_user(&new_ka.sa.sa_mask, &act->sa_mask, sizeof(compat_sigset_t));
		if (ret)
			return -EFAULT;

		sigact_set_handler(&new_ka, handler, restorer);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		ret = put_user(IA32_SA_HANDLER(&old_ka), &oact->sa_handler);
		ret |= put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		ret |= put_user(IA32_SA_RESTORER(&old_ka), &oact->sa_restorer);
		ret |= copy_to_user(&oact->sa_mask, &old_ka.sa.sa_mask, sizeof(compat_sigset_t));
	}
	return ret;
}


asmlinkage long
sys32_rt_sigprocmask (int how, compat_sigset_t *set, compat_sigset_t *oset, unsigned int sigsetsize)
{
	mm_segment_t old_fs = get_fs();
	sigset_t s;
	long ret;

	if (sigsetsize > sizeof(s))
		return -EINVAL;

	if (set) {
		memset(&s, 0, sizeof(s));
		if (copy_from_user(&s.sig, set, sigsetsize))
			return -EFAULT;
	}
	set_fs(KERNEL_DS);
	ret = sys_rt_sigprocmask(how, set ? &s : NULL, oset ? &s : NULL, sizeof(s));
	set_fs(old_fs);
	if (ret)
		return ret;
	if (oset) {
		if (copy_to_user(oset, &s.sig, sigsetsize))
			return -EFAULT;
	}
	return 0;
}

asmlinkage long
sys32_rt_sigtimedwait (compat_sigset_t *uthese, siginfo_t32 *uinfo,
		struct compat_timespec *uts, unsigned int sigsetsize)
{
	extern int copy_siginfo_to_user32 (siginfo_t32 *, siginfo_t *);
	mm_segment_t old_fs = get_fs();
	struct timespec t;
	siginfo_t info;
	sigset_t s;
	int ret;

	if (copy_from_user(&s.sig, uthese, sizeof(compat_sigset_t)))
		return -EFAULT;
	if (uts && get_compat_timespec(&t, uts))
		return -EFAULT;
	set_fs(KERNEL_DS);
	ret = sys_rt_sigtimedwait(&s, uinfo ? &info : NULL, uts ? &t : NULL,
			sigsetsize);
	set_fs(old_fs);
	if (ret >= 0 && uinfo) {
		if (copy_siginfo_to_user32(uinfo, &info))
			return -EFAULT;
	}
	return ret;
}

asmlinkage long
sys32_rt_sigqueueinfo (int pid, int sig, siginfo_t32 *uinfo)
{
	extern int copy_siginfo_from_user32 (siginfo_t *to, siginfo_t32 *from);
	mm_segment_t old_fs = get_fs();
	siginfo_t info;
	int ret;

	if (copy_siginfo_from_user32(&info, uinfo))
		return -EFAULT;
	set_fs(KERNEL_DS);
	ret = sys_rt_sigqueueinfo(pid, sig, &info);
	set_fs(old_fs);
	return ret;
}

asmlinkage long
sys32_sigaction (int sig, struct old_sigaction32 *act, struct old_sigaction32 *oact)
{
	struct k_sigaction new_ka, old_ka;
	unsigned int handler, restorer;
	int ret;

	if (act) {
		compat_old_sigset_t mask;

		ret = get_user(handler, &act->sa_handler);
		ret |= get_user(new_ka.sa.sa_flags, &act->sa_flags);
		ret |= get_user(restorer, &act->sa_restorer);
		ret |= get_user(mask, &act->sa_mask);
		if (ret)
			return ret;

		sigact_set_handler(&new_ka, handler, restorer);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		ret = put_user(IA32_SA_HANDLER(&old_ka), &oact->sa_handler);
		ret |= put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		ret |= put_user(IA32_SA_RESTORER(&old_ka), &oact->sa_restorer);
		ret |= put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

static int
setup_sigcontext_ia32 (struct sigcontext_ia32 *sc, struct _fpstate_ia32 *fpstate,
		       struct pt_regs *regs, unsigned long mask)
{
	int  err = 0;
	unsigned long flag;

	if (!access_ok(VERIFY_WRITE, sc, sizeof(*sc)))
		return -EFAULT;

	err |= __put_user((regs->r16 >> 32) & 0xffff, (unsigned int *)&sc->fs);
	err |= __put_user((regs->r16 >> 48) & 0xffff, (unsigned int *)&sc->gs);
	err |= __put_user((regs->r16 >> 16) & 0xffff, (unsigned int *)&sc->es);
	err |= __put_user(regs->r16 & 0xffff, (unsigned int *)&sc->ds);
	err |= __put_user(regs->r15, &sc->edi);
	err |= __put_user(regs->r14, &sc->esi);
	err |= __put_user(regs->r13, &sc->ebp);
	err |= __put_user(regs->r12, &sc->esp);
	err |= __put_user(regs->r11, &sc->ebx);
	err |= __put_user(regs->r10, &sc->edx);
	err |= __put_user(regs->r9, &sc->ecx);
	err |= __put_user(regs->r8, &sc->eax);
#if 0
	err |= __put_user(current->tss.trap_no, &sc->trapno);
	err |= __put_user(current->tss.error_code, &sc->err);
#endif
	err |= __put_user(regs->cr_iip, &sc->eip);
	err |= __put_user(regs->r17 & 0xffff, (unsigned int *)&sc->cs);
	/*
	 *  `eflags' is in an ar register for this context
	 */
	flag = ia64_getreg(_IA64_REG_AR_EFLAG);
	err |= __put_user((unsigned int)flag, &sc->eflags);
	err |= __put_user(regs->r12, &sc->esp_at_signal);
	err |= __put_user((regs->r17 >> 16) & 0xffff, (unsigned int *)&sc->ss);

	if ( save_ia32_fpstate_live(fpstate) < 0 )
		err = -EFAULT;
	else
		err |= __put_user((u32)(u64)fpstate, &sc->fpstate);

#if 0
	tmp = save_i387(fpstate);
	if (tmp < 0)
		err = 1;
	else
		err |= __put_user(tmp ? fpstate : NULL, &sc->fpstate);

	/* non-iBCS2 extensions.. */
#endif
	err |= __put_user(mask, &sc->oldmask);
#if 0
	err |= __put_user(current->tss.cr2, &sc->cr2);
#endif
	return err;
}

static int
restore_sigcontext_ia32 (struct pt_regs *regs, struct sigcontext_ia32 *sc, int *peax)
{
	unsigned int err = 0;

	/* Always make any pending restarted system calls return -EINTR */
	current_thread_info()->restart_block.fn = do_no_restart_syscall;

	if (!access_ok(VERIFY_READ, sc, sizeof(*sc)))
		return(-EFAULT);

#define COPY(ia64x, ia32x)	err |= __get_user(regs->ia64x, &sc->ia32x)

#define copyseg_gs(tmp)		(regs->r16 |= (unsigned long) (tmp) << 48)
#define copyseg_fs(tmp)		(regs->r16 |= (unsigned long) (tmp) << 32)
#define copyseg_cs(tmp)		(regs->r17 |= tmp)
#define copyseg_ss(tmp)		(regs->r17 |= (unsigned long) (tmp) << 16)
#define copyseg_es(tmp)		(regs->r16 |= (unsigned long) (tmp) << 16)
#define copyseg_ds(tmp)		(regs->r16 |= tmp)

#define COPY_SEG(seg)					\
	{						\
		unsigned short tmp;			\
		err |= __get_user(tmp, &sc->seg);	\
		copyseg_##seg(tmp);			\
	}
#define COPY_SEG_STRICT(seg)				\
	{						\
		unsigned short tmp;			\
		err |= __get_user(tmp, &sc->seg);	\
		copyseg_##seg(tmp|3);			\
	}

	/* To make COPY_SEGs easier, we zero r16, r17 */
	regs->r16 = 0;
	regs->r17 = 0;

	COPY_SEG(gs);
	COPY_SEG(fs);
	COPY_SEG(es);
	COPY_SEG(ds);
	COPY(r15, edi);
	COPY(r14, esi);
	COPY(r13, ebp);
	COPY(r12, esp);
	COPY(r11, ebx);
	COPY(r10, edx);
	COPY(r9, ecx);
	COPY(cr_iip, eip);
	COPY_SEG_STRICT(cs);
	COPY_SEG_STRICT(ss);
	ia32_load_segment_descriptors(current);
	{
		unsigned int tmpflags;
		unsigned long flag;

		/*
		 *  IA32 `eflags' is not part of `pt_regs', it's in an ar register which
		 *  is part of the thread context.  Fortunately, we are executing in the
		 *  IA32 process's context.
		 */
		err |= __get_user(tmpflags, &sc->eflags);
		flag = ia64_getreg(_IA64_REG_AR_EFLAG);
		flag &= ~0x40DD5;
		flag |= (tmpflags & 0x40DD5);
		ia64_setreg(_IA64_REG_AR_EFLAG, flag);

		regs->r1 = -1;	/* disable syscall checks, r1 is orig_eax */
	}

	{
		struct _fpstate_ia32 *buf = NULL;
		u32    fpstate_ptr;
		err |= get_user(fpstate_ptr, &(sc->fpstate));
		buf = (struct _fpstate_ia32 *)(u64)fpstate_ptr;
		if (buf) {
			err |= restore_ia32_fpstate_live(buf);
		}
	}

#if 0
	{
		struct _fpstate * buf;
		err |= __get_user(buf, &sc->fpstate);
		if (buf) {
			if (verify_area(VERIFY_READ, buf, sizeof(*buf)))
				goto badframe;
			err |= restore_i387(buf);
		}
	}
#endif

	err |= __get_user(*peax, &sc->eax);
	return err;

#if 0
  badframe:
	return 1;
#endif
}

/*
 * Determine which stack to use..
 */
static inline void *
get_sigframe (struct k_sigaction *ka, struct pt_regs * regs, size_t frame_size)
{
	unsigned long esp;

	/* Default to using normal stack (truncate off sign-extension of bit 31: */
	esp = (unsigned int) regs->r12;

	/* This is the X/Open sanctioned signal stack switching.  */
	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (!on_sig_stack(esp))
			esp = current->sas_ss_sp + current->sas_ss_size;
	}
	/* Legacy stack switching not supported */

	return (void *)((esp - frame_size) & -8ul);
}

static int
setup_frame_ia32 (int sig, struct k_sigaction *ka, sigset_t *set, struct pt_regs * regs)
{
	struct exec_domain *ed = current_thread_info()->exec_domain;
	struct sigframe_ia32 *frame;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (!access_ok(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= __put_user((ed && ed->signal_invmap && sig < 32
			   ? (int)(ed->signal_invmap[sig]) : sig), &frame->sig);

	err |= setup_sigcontext_ia32(&frame->sc, &frame->fpstate, regs, set->sig[0]);

	if (_COMPAT_NSIG_WORDS > 1)
		err |= __copy_to_user(frame->extramask, (char *) &set->sig + 4,
				      sizeof(frame->extramask));

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		unsigned int restorer = IA32_SA_RESTORER(ka);
		err |= __put_user(restorer, &frame->pretcode);
	} else {
		err |= __put_user((long)frame->retcode, &frame->pretcode);
		/* This is popl %eax ; movl $,%eax ; int $0x80 */
		err |= __put_user(0xb858, (short *)(frame->retcode+0));
		err |= __put_user(__IA32_NR_sigreturn & 0xffff, (short *)(frame->retcode+2));
		err |= __put_user(__IA32_NR_sigreturn >> 16, (short *)(frame->retcode+4));
		err |= __put_user(0x80cd, (short *)(frame->retcode+6));
	}

	if (err)
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->r12 = (unsigned long) frame;
	regs->cr_iip = IA32_SA_HANDLER(ka);

	set_fs(USER_DS);

#if 0
	regs->eflags &= ~TF_MASK;
#endif

#if 0
	printk("SIG deliver (%s:%d): sig=%d sp=%p pc=%lx ra=%x\n",
               current->comm, current->pid, sig, (void *) frame, regs->cr_iip, frame->pretcode);
#endif

	return 1;

  give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
	return 0;
}

static int
setup_rt_frame_ia32 (int sig, struct k_sigaction *ka, siginfo_t *info,
		     sigset_t *set, struct pt_regs * regs)
{
	struct exec_domain *ed = current_thread_info()->exec_domain;
	struct rt_sigframe_ia32 *frame;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (!access_ok(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= __put_user((ed && ed->signal_invmap
			   && sig < 32 ? ed->signal_invmap[sig] : sig), &frame->sig);
	err |= __put_user((long)&frame->info, &frame->pinfo);
	err |= __put_user((long)&frame->uc, &frame->puc);
	err |= copy_siginfo_to_user32(&frame->info, info);

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __put_user(current->sas_ss_sp, &frame->uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->r12), &frame->uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &frame->uc.uc_stack.ss_size);
	err |= setup_sigcontext_ia32(&frame->uc.uc_mcontext, &frame->fpstate, regs, set->sig[0]);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		unsigned int restorer = IA32_SA_RESTORER(ka);
		err |= __put_user(restorer, &frame->pretcode);
	} else {
		err |= __put_user((long)frame->retcode, &frame->pretcode);
		/* This is movl $,%eax ; int $0x80 */
		err |= __put_user(0xb8, (char *)(frame->retcode+0));
		err |= __put_user(__IA32_NR_rt_sigreturn, (int *)(frame->retcode+1));
		err |= __put_user(0x80cd, (short *)(frame->retcode+5));
	}

	if (err)
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->r12 = (unsigned long) frame;
	regs->cr_iip = IA32_SA_HANDLER(ka);

	set_fs(USER_DS);

#if 0
	regs->eflags &= ~TF_MASK;
#endif

#if 0
	printk("SIG deliver (%s:%d): sp=%p pc=%lx ra=%x\n",
               current->comm, current->pid, (void *) frame, regs->cr_iip, frame->pretcode);
#endif

	return 1;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
	return 0;
}

int
ia32_setup_frame1 (int sig, struct k_sigaction *ka, siginfo_t *info,
		   sigset_t *set, struct pt_regs *regs)
{
       /* Set up the stack frame */
       if (ka->sa.sa_flags & SA_SIGINFO)
               return setup_rt_frame_ia32(sig, ka, info, set, regs);
       else
               return setup_frame_ia32(sig, ka, set, regs);
}

asmlinkage long
sys32_sigreturn (int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
		 unsigned long stack)
{
	struct pt_regs *regs = (struct pt_regs *) &stack;
	unsigned long esp = (unsigned int) regs->r12;
	struct sigframe_ia32 *frame = (struct sigframe_ia32 *)(esp - 8);
	sigset_t set;
	int eax;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;

	if (__get_user(set.sig[0], &frame->sc.oldmask)
	    || (_COMPAT_NSIG_WORDS > 1 && __copy_from_user((char *) &set.sig + 4, &frame->extramask,
							 sizeof(frame->extramask))))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked = (sigset_t) set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	if (restore_sigcontext_ia32(regs, &frame->sc, &eax))
		goto badframe;
	return eax;

  badframe:
	force_sig(SIGSEGV, current);
	return 0;
}

asmlinkage long
sys32_rt_sigreturn (int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
		    unsigned long stack)
{
	struct pt_regs *regs = (struct pt_regs *) &stack;
	unsigned long esp = (unsigned int) regs->r12;
	struct rt_sigframe_ia32 *frame = (struct rt_sigframe_ia32 *)(esp - 4);
	sigset_t set;
	stack_t st;
	int eax;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked =  set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	if (restore_sigcontext_ia32(regs, &frame->uc.uc_mcontext, &eax))
		goto badframe;

	if (__copy_from_user(&st, &frame->uc.uc_stack, sizeof(st)))
		goto badframe;
	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, esp);

	return eax;

  badframe:
	force_sig(SIGSEGV, current);
	return 0;
}
