/*
 *  linux/include/asm-arm/tlb.h
 *
 *  Copyright (C) 2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Experimentation shows that on a StrongARM, it appears to be faster
 *  to use the "invalidate whole tlb" rather than "invalidate single
 *  tlb" for this.
 *
 *  This appears true for both the process fork+exit case, as well as
 *  the munmap-large-area case.
 */
#ifndef __ASMARM_TLB_H
#define __ASMARM_TLB_H

#include <asm/tlbflush.h>

/*
 * TLB handling.  This allows us to remove pages from the page
 * tables, and efficiently handle the TLB issues.
 */
typedef struct free_pte_ctx {
	struct mm_struct	*mm;
	unsigned int		freed;

	unsigned int		flushes;
	unsigned int		avoided_flushes;
} mmu_gather_t;

extern mmu_gather_t mmu_gathers[NR_CPUS];

static inline mmu_gather_t *tlb_gather_mmu(struct mm_struct *mm, unsigned int full_mm_flush)
{
	int cpu = smp_processor_id();
	mmu_gather_t *tlb = &mmu_gathers[cpu];

	tlb->mm = mm;
	tlb->freed = 0;

	return tlb;
}

static inline void tlb_finish_mmu(mmu_gather_t *tlb, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = tlb->mm;
	unsigned long freed = tlb->freed;
	int rss = mm->rss;

	if (rss < freed)
		freed = rss;
	mm->rss = rss - freed;

	if (freed) {
		flush_tlb_mm(mm);
		tlb->flushes++;
	} else {
		tlb->avoided_flushes++;
	}

	/* keep the page table cache within bounds */
	check_pgt_cache();
}

#define tlb_remove_tlb_entry(tlb,ptep,address)	do { } while (0)
#define tlb_start_vma(tlb,vma)			do { } while (0)
#define tlb_end_vma(tlb,vma)			do { } while (0)

#define tlb_remove_page(tlb,page)	free_page_and_swap_cache(page)
#define pte_free_tlb(tlb,ptep)		pte_free(ptep)
#define pmd_free_tlb(tlb,pmdp)		pmd_free(pmdp)

#endif
