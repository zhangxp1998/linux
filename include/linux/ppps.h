/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PPPS_H
#define _LINUX_PPPS_H

#include <asm/page.h>

#ifndef __ASSEMBLY__
#include <asm/current.h>

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define PAGE_SHIFT_COMPAT	12
#define VA_BITS_COMPAT		39
#define _MM_PAGE_SHIFT_HELPER(mm) \
	((mm) && (mm)->page_shift ? (mm)->page_shift : PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm) \
	(((mm) && (mm)->page_shift == PAGE_SHIFT_COMPAT) ? VA_BITS_COMPAT : VA_BITS)
#define ppps_mm_is_compat(mm)						\
	((mm) && (mm)->page_shift == PAGE_SHIFT_COMPAT)

#else
#define PAGE_SHIFT_COMPAT	PAGE_SHIFT
#define VA_BITS_COMPAT		VA_BITS
#define PGTABLE_MM()		(NULL)
#define _MM_PAGE_SHIFT_HELPER(mm) \
	((void)(mm), PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm)		((void)(mm), VA_BITS)
#define ppps_mm_is_compat(mm)		((void)(mm), false)

#endif

#define MM_PAGE_SHIFT(...) \
	_MM_PAGE_SHIFT_DISPATCH(__VA_ARGS__ __VA_OPT__(, /* */) PGTABLE_MM())
#define _MM_PAGE_SHIFT_DISPATCH(a, ...) \
	_MM_PAGE_SHIFT_HELPER(a)

#define MM_VA_BITS(...) \
	_MM_VA_BITS_DISPATCH(__VA_ARGS__ __VA_OPT__(, /* */) PGTABLE_MM())
#define _MM_VA_BITS_DISPATCH(a, ...) \
	_MM_VA_BITS_HELPER(a)

#define PAGE_SIZE_COMPAT	(1UL << PAGE_SHIFT_COMPAT)
#define PAGE_MASK_COMPAT	(~(PAGE_SIZE_COMPAT - 1))

#define MM_PAGE_SIZE(...)	(1UL << MM_PAGE_SHIFT(__VA_ARGS__))
#define MM_PAGE_MASK(...)	(~(MM_PAGE_SIZE(__VA_ARGS__) - 1))

#define MM_LEVEL_SHIFT(...)	(MM_PAGE_SHIFT(__VA_ARGS__) - 3)

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define MM_PMD_SHIFT(...)	(MM_PAGE_SHIFT(__VA_ARGS__) +  MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_PUD_SHIFT(...)	(MM_PMD_SHIFT(__VA_ARGS__) +  MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_P4D_SHIFT(...)	(MM_PUD_SHIFT(__VA_ARGS__) +  MM_LEVEL_SHIFT(__VA_ARGS__))

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
#define MM_PGD_SHIFT(...)	(MM_P4D_SHIFT(__VA_ARGS__) +  MM_LEVEL_SHIFT(__VA_ARGS__))
#endif

#define MM_PTRS_PER_PTE(...)	(1UL << MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_PTRS_PER_PMD(...)	(1UL << MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_PTRS_PER_PUD(...)	(1UL << MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_PTRS_PER_P4D(...)	(1UL << MM_LEVEL_SHIFT(__VA_ARGS__))
#define MM_PTRS_PER_PGD(...)	(1UL << (MM_VA_BITS(__VA_ARGS__) - MM_PGD_SHIFT(__VA_ARGS__)))

#define MM_PMD_SIZE(...)	(1UL << MM_PMD_SHIFT(__VA_ARGS__))
#define MM_PMD_MASK(...)	(~(MM_PMD_SIZE(__VA_ARGS__) - 1))
#define MM_PUD_SIZE(...)	(1UL << MM_PUD_SHIFT(__VA_ARGS__))
#define MM_PUD_MASK(...)	(~(MM_PUD_SIZE(__VA_ARGS__) - 1))
#define MM_PGDIR_SIZE(...)	(1UL << MM_PGD_SHIFT(__VA_ARGS__))
#define MM_PGDIR_MASK(...)	(~(MM_PGDIR_SIZE(__VA_ARGS__) - 1))
#define MM_P4D_SIZE(...)	(1UL << MM_P4D_SHIFT(__VA_ARGS__))
#define MM_P4D_MASK(...)	(~(MM_P4D_SIZE(__VA_ARGS__) - 1))
#else
#define MM_PMD_SHIFT(mm)	((void)(mm), PMD_SHIFT)
#define MM_PUD_SHIFT(mm)	((void)(mm), PUD_SHIFT)
#define MM_P4D_SHIFT(mm)	((void)(mm), P4D_SHIFT)
#define MM_PGD_SHIFT(mm)	((void)(mm), PGDIR_SHIFT)

#define MM_PTRS_PER_PTE(mm)	((void)(mm), PTRS_PER_PTE)
#define MM_PTRS_PER_PMD(mm)	((void)(mm), PTRS_PER_PMD)
#define MM_PTRS_PER_PUD(mm)	((void)(mm), PTRS_PER_PUD)
#define MM_PTRS_PER_P4D(mm)	((void)(mm), PTRS_PER_P4D)
#define MM_PTRS_PER_PGD(mm)	((void)(mm), PTRS_PER_PGD)

#define MM_PMD_SIZE(mm)	((void)(mm), PMD_SIZE)
#define MM_PMD_MASK(mm)	((void)(mm), PMD_MASK)
#define MM_PUD_SIZE(mm)	((void)(mm), PUD_SIZE)
#define MM_PUD_MASK(mm)	((void)(mm), PUD_MASK)
#define MM_PGDIR_SIZE(mm)	((void)(mm), PGDIR_SIZE)
#define MM_PGDIR_MASK(mm)	((void)(mm), PGDIR_MASK)
#define MM_P4D_SIZE(mm)	((void)(mm), P4D_SIZE)
#define MM_P4D_MASK(mm)	((void)(mm), P4D_MASK)
#endif

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
