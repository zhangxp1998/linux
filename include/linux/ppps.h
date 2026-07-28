/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PPPS_H
#define _LINUX_PPPS_H

#include <asm/page.h>

#ifndef __ASSEMBLY__
#include <asm/current.h>

struct linux_binprm;

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define PAGE_SHIFT_COMPAT	12
#define VA_BITS_COMPAT		39
#define _MM_PAGE_SHIFT_HELPER(mm) \
	(((struct mm_struct *)(mm)) && ((struct mm_struct *)(mm))->page_shift ? ((struct mm_struct *)(mm))->page_shift : PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm) \
	((((struct mm_struct *)(mm)) && ((struct mm_struct *)(mm))->page_shift == PAGE_SHIFT_COMPAT) ? VA_BITS_COMPAT : VA_BITS)
#define ppps_mm_is_compat(mm)						\
	(((struct mm_struct *)(mm)) && ((struct mm_struct *)(mm))->page_shift == PAGE_SHIFT_COMPAT)

unsigned long mm_task_size64(void);
unsigned long mm_task_size64_of(struct mm_struct *mm);
unsigned long mm_default_map_window64(void);
unsigned long mm_default_map_window64_of(struct mm_struct *mm);

void mm_init_pagesize(struct mm_struct *mm, struct linux_binprm *bprm);

#define mm_set_bprm_exec(bprm)		(current->bprm_exec = (bprm))
#define mm_clear_bprm_exec()		(current->bprm_exec = NULL)
#define mm_get_bprm_exec()		(current->bprm_exec)
#define vma_set_slice_off(vma, val)	((vma)->vm_slice_off = (val))
#define vma_slice_off(vma)		((vma)->vm_slice_off)
#define vma_page_shift(vma) \
	((ppps_mm_is_compat((vma)->vm_mm) && !(vma)->vm_ops) ? \
	 PAGE_SHIFT_COMPAT : PAGE_SHIFT)

#define vma_slice_shift(vma) \
	((ppps_mm_is_compat((vma)->vm_mm) && !(vma)->vm_ops) ? \
	 PPPS_SLICE_SHIFT : 0)
/*
 * clear_pte_slice_offset - Clear any existing subpage offset bits in the PTE
 *
 * In a host page, the address bits between PAGE_SIZE_COMPAT and PAGE_SIZE
 * track the subpage slice offset. Clear these bits from the physical address
 * field of the PTE before applying a new subpage slice offset.
 */
#define clear_pte_slice_offset(pte)					\
	__pte(pte_val(pte) & ~((PAGE_SIZE - 1) & ~(PAGE_SIZE_COMPAT - 1)))
#else
#define PAGE_SHIFT_COMPAT	PAGE_SHIFT
#define VA_BITS_COMPAT		VA_BITS
#define PGTABLE_MM()		(NULL)
#define _MM_PAGE_SHIFT_HELPER(mm) \
	((void)(mm), PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm)		((void)(mm), VA_BITS)
#define ppps_mm_is_compat(mm)		((void)(mm), false)

#define mm_task_size64()		(1UL << vabits_actual)
#define mm_task_size64_of(mm)		((void)(mm), (1UL << vabits_actual))
#define mm_default_map_window64()	(1UL << VA_BITS_MIN)
#define mm_default_map_window64_of(mm)	((void)(mm), (1UL << VA_BITS_MIN))

static inline void mm_init_pagesize(struct mm_struct *mm, struct linux_binprm *bprm) {}
#define mm_set_bprm_exec(bprm)		((void)(bprm))
#define mm_clear_bprm_exec()		do { } while (0)
#define mm_get_bprm_exec()		(NULL)
#define vma_set_slice_off(vma, val)	((void)(vma), (void)(val))
#define vma_slice_off(vma)		((void)(vma), 0)
#define vma_page_shift(vma)		((void)(vma), PAGE_SHIFT)
#define vma_slice_shift(vma)		((void)(vma), 0)
#endif

#define PPPS_SLICE_SHIFT	(PAGE_SHIFT - PAGE_SHIFT_COMPAT)
#define PPPS_SLICES_PER_PAGE	(1UL << PPPS_SLICE_SHIFT)
#define PPPS_SLICE_MASK		(PPPS_SLICES_PER_PAGE - 1)

#define MM_PAGE_SHIFT(mm)	_MM_PAGE_SHIFT_HELPER(mm)
#define MM_VA_BITS(mm)		_MM_VA_BITS_HELPER(mm)

#define PAGE_SIZE_COMPAT	(1UL << PAGE_SHIFT_COMPAT)
#define PAGE_MASK_COMPAT	(~(PAGE_SIZE_COMPAT - 1))

#define LEVEL_SHIFT_COMPAT	(PAGE_SHIFT_COMPAT - 3)

#define PMD_SHIFT_COMPAT	(PAGE_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)
#define PUD_SHIFT_COMPAT	(PMD_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)
#define P4D_SHIFT_COMPAT	(PUD_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)

#if CONFIG_PGTABLE_LEVELS == 2
#define PGD_SHIFT_COMPAT	PMD_SHIFT_COMPAT
#elif CONFIG_PGTABLE_LEVELS == 3
#define PGD_SHIFT_COMPAT	PUD_SHIFT_COMPAT
#elif CONFIG_PGTABLE_LEVELS == 4
#define PGD_SHIFT_COMPAT	P4D_SHIFT_COMPAT
#elif CONFIG_PGTABLE_LEVELS == 5
#define PGD_SHIFT_COMPAT	(P4D_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)
#endif

#define PTRS_PER_PTE_COMPAT	(1UL << LEVEL_SHIFT_COMPAT)
#define PTRS_PER_PMD_COMPAT	(1UL << LEVEL_SHIFT_COMPAT)
#define PTRS_PER_PUD_COMPAT	(1UL << LEVEL_SHIFT_COMPAT)
#define PTRS_PER_P4D_COMPAT	(1UL << LEVEL_SHIFT_COMPAT)
#define PTRS_PER_PGD_COMPAT	(1UL << (VA_BITS_COMPAT - PGD_SHIFT_COMPAT))

#define PMD_SIZE_COMPAT		(1UL << PMD_SHIFT_COMPAT)
#define PMD_MASK_COMPAT		(~(PMD_SIZE_COMPAT - 1))
#define PUD_SIZE_COMPAT		(1UL << PUD_SHIFT_COMPAT)
#define PUD_MASK_COMPAT		(~(PUD_SIZE_COMPAT - 1))
#define PGDIR_SIZE_COMPAT	(1UL << PGD_SHIFT_COMPAT)
#define PGDIR_MASK_COMPAT	(~(PGDIR_SIZE_COMPAT - 1))

#define PTE_ADDR_LOW_COMPAT \
	(((_AT(pteval_t, 1) << (50 - PAGE_SHIFT_COMPAT)) - 1) \
	 << PAGE_SHIFT_COMPAT)


#define MM_PAGE_SIZE(mm)	(1UL << MM_PAGE_SHIFT(mm))
#define MM_PAGE_MASK(mm)	(~(MM_PAGE_SIZE(mm) - 1))

#define MM_LEVEL_SHIFT(mm)	(MM_PAGE_SHIFT(mm) - 3)

#define MM_PAGE_ALIGN(mm, addr)	ALIGN(addr, MM_PAGE_SIZE(mm))
#define MM_PAGE_ALIGNED(mm, addr)	(!((addr) & ~MM_PAGE_MASK(mm)))
#define MM_PHYS_PFN(mm, x)		((x) >> MM_PAGE_SHIFT(mm))
#define mm_offset_in_page(mm, p)	((unsigned long)(p) & ~MM_PAGE_MASK(mm))

#define MM_PMD_SHIFT(...)	(MM_PAGE_SHIFT(__VA_ARGS__) + MM_LEVEL_SHIFT(__VA_ARGS__))

#if CONFIG_PGTABLE_LEVELS > 2
#define MM_PUD_SHIFT(mm)	(MM_PMD_SHIFT(mm) + MM_LEVEL_SHIFT(mm))
#else
#define MM_PUD_SHIFT(mm)	MM_PMD_SHIFT(mm)
#endif

#if CONFIG_PGTABLE_LEVELS > 3
#define MM_P4D_SHIFT(mm)	(MM_PUD_SHIFT(mm) + MM_LEVEL_SHIFT(mm))
#else
#define MM_P4D_SHIFT(mm)	MM_PUD_SHIFT(mm)
#endif

/*
 * We currently only support a 3-level page table setup. Other levels
 * are defined generically here for completeness and folded as needed.
 */
#if CONFIG_PGTABLE_LEVELS == 2
#define MM_PGD_SHIFT(mm)	MM_PMD_SHIFT(mm)
#elif CONFIG_PGTABLE_LEVELS == 3
#define MM_PGD_SHIFT(mm)	MM_PUD_SHIFT(mm)
#elif CONFIG_PGTABLE_LEVELS == 4
#define MM_PGD_SHIFT(mm)	MM_P4D_SHIFT(mm)
#elif CONFIG_PGTABLE_LEVELS == 5
#define MM_PGD_SHIFT(mm)	(MM_P4D_SHIFT(mm) + MM_LEVEL_SHIFT(mm))
#endif

#define MM_PTRS_PER_PTE(mm)	(1UL << MM_LEVEL_SHIFT(mm))
#if CONFIG_PGTABLE_LEVELS > 2
#define MM_PTRS_PER_PMD(mm)	(1UL << MM_LEVEL_SHIFT(mm))
#else
#define MM_PTRS_PER_PMD(mm)	((void)(mm), 1UL)
#endif
#if CONFIG_PGTABLE_LEVELS > 3
#define MM_PTRS_PER_PUD(mm)	(1UL << MM_LEVEL_SHIFT(mm))
#else
#define MM_PTRS_PER_PUD(mm)	((void)(mm), 1UL)
#endif
#if CONFIG_PGTABLE_LEVELS > 4
#define MM_PTRS_PER_P4D(mm)	(1UL << MM_LEVEL_SHIFT(mm))
#else
#define MM_PTRS_PER_P4D(mm)	((void)(mm), 1UL)
#endif
#define MM_PTRS_PER_PGD(...)	(1UL << (MM_VA_BITS(__VA_ARGS__) - MM_PGD_SHIFT(__VA_ARGS__)))

#define MM_PMD_SIZE(...)	(1UL << MM_PMD_SHIFT(__VA_ARGS__))
#define MM_PMD_MASK(...)	(~(MM_PMD_SIZE(__VA_ARGS__) - 1))
#define MM_PUD_SIZE(...)	(1UL << MM_PUD_SHIFT(__VA_ARGS__))
#define MM_PUD_MASK(...)	(~(MM_PUD_SIZE(__VA_ARGS__) - 1))
#define MM_PGDIR_SIZE(...)	(1UL << MM_PGD_SHIFT(__VA_ARGS__))
#define MM_PGDIR_MASK(...)	(~(MM_PGDIR_SIZE(__VA_ARGS__) - 1))
#define MM_P4D_SIZE(...)	(1UL << MM_P4D_SHIFT(__VA_ARGS__))
#define MM_P4D_MASK(...)	(~(MM_P4D_SIZE(__VA_ARGS__) - 1))

#define IS_KERNEL_ADDR(addr)	((long)(addr) < 0)

#define __MM_ADDR_EVAL(addr, kern_val, mm_val) \
	(IS_KERNEL_ADDR(addr) ? (kern_val) : (mm_val))

#define MM_ADDR_PAGE_SHIFT(addr, mm) \
	__MM_ADDR_EVAL(addr, PAGE_SHIFT, MM_PAGE_SHIFT(mm))

#define MM_ADDR_PAGE_SIZE(addr, mm) \
	__MM_ADDR_EVAL(addr, PAGE_SIZE, MM_PAGE_SIZE(mm))

#define MM_ADDR_PAGE_MASK(addr, mm) \
	__MM_ADDR_EVAL(addr, PAGE_MASK, MM_PAGE_MASK(mm))

#define MM_ADDR_PMD_SHIFT(addr, mm) \
	__MM_ADDR_EVAL(addr, PMD_SHIFT, MM_PMD_SHIFT(mm))

#define MM_ADDR_PMD_SIZE(addr, mm) \
	__MM_ADDR_EVAL(addr, PMD_SIZE, MM_PMD_SIZE(mm))

#define MM_ADDR_PMD_MASK(addr, mm) \
	__MM_ADDR_EVAL(addr, PMD_MASK, MM_PMD_MASK(mm))

#define MM_ADDR_PUD_SHIFT(addr, mm) \
	__MM_ADDR_EVAL(addr, PUD_SHIFT, MM_PUD_SHIFT(mm))

#define MM_ADDR_PUD_SIZE(addr, mm) \
	__MM_ADDR_EVAL(addr, PUD_SIZE, MM_PUD_SIZE(mm))

#define MM_ADDR_PUD_MASK(addr, mm) \
	__MM_ADDR_EVAL(addr, PUD_MASK, MM_PUD_MASK(mm))

#define MM_ADDR_P4D_SHIFT(addr, mm) \
	__MM_ADDR_EVAL(addr, P4D_SHIFT, MM_P4D_SHIFT(mm))

#define MM_ADDR_P4D_SIZE(addr, mm) \
	__MM_ADDR_EVAL(addr, P4D_SIZE, MM_P4D_SIZE(mm))

#define MM_ADDR_P4D_MASK(addr, mm) \
	__MM_ADDR_EVAL(addr, P4D_MASK, MM_P4D_MASK(mm))

#define MM_ADDR_PGD_SHIFT(addr, mm) \
	__MM_ADDR_EVAL(addr, PGDIR_SHIFT, MM_PGD_SHIFT(mm))

#define MM_ADDR_PGDIR_SIZE(addr, mm) \
	__MM_ADDR_EVAL(addr, PGDIR_SIZE, MM_PGDIR_SIZE(mm))

#define MM_ADDR_PGDIR_MASK(addr, mm) \
	__MM_ADDR_EVAL(addr, PGDIR_MASK, MM_PGDIR_MASK(mm))

#define MM_ADDR_PTRS_PER_PTE(addr, mm) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PTE, MM_PTRS_PER_PTE(mm))

#define MM_ADDR_PTRS_PER_PMD(addr, mm) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PMD, MM_PTRS_PER_PMD(mm))

#define MM_ADDR_PTRS_PER_PUD(addr, mm) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PUD, MM_PTRS_PER_PUD(mm))

#define MM_ADDR_PTRS_PER_P4D(addr, mm) \
	__MM_ADDR_EVAL(addr, PTRS_PER_P4D, MM_PTRS_PER_P4D(mm))

#define MM_ADDR_PTRS_PER_PGD(addr, mm) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PGD, MM_PTRS_PER_PGD(mm))

#endif /* __ASSEMBLY__ */

#endif /* _LINUX_PPPS_H */
