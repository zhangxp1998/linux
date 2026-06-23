/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PPPS_H
#define _LINUX_PPPS_H

#include <asm/page.h>
#include <asm/current.h>

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define PAGE_SHIFT_COMPAT	12
#define VA_BITS_COMPAT		39
#define PGTABLE_MM() \
	((current && current->pgtable_mm) ? \
	 current->pgtable_mm : (current ? current->active_mm : NULL))
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

#endif /* _LINUX_PPPS_H */
