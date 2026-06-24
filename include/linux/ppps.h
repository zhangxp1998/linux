/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-Process Page Size (PPPS)
 *
 * The kernel runs with the native page size (16K on the supported arm64
 * configuration).  A process that execs with the ADDR_4KB_COMPAT_PAGE_SIZE
 * personality bit becomes a "compat" process: its mm has page_shift ==
 * PAGE_SHIFT_COMPAT and its page tables are walked with 4K geometry.  Every
 * user-visible boundary (mmap/mprotect/madvise alignment, /proc page counts,
 * ...) uses the process page size; folios, the page cache and rmap stay
 * native.  PPPS provides only the minimal 4K interface user space needs to
 * run; whatever is not part of that interface keeps native 16K behaviour.
 *
 * Vocabulary
 *   native page   PAGE_SIZE bytes, one struct page / folio.
 *   process page  MM_PAGE_SIZE(mm) bytes, one PTE of that mm.
 *   slice         one process page inside a native page, index
 *                 0..PPPS_SLICES_PER_PAGE-1.  A compat PTE maps one slice,
 *                 encoded in the PTE address bits between PAGE_SHIFT_COMPAT
 *                 and PAGE_SHIFT (see pte_mkslice()).
 *   tuple         the PPPS_SLICES_PER_PAGE consecutive compat PTEs that
 *                 share one anonymous native folio (mm/ppps_anon.c).
 *
 * VMA bookkeeping (see vma_address_to_slice(), vma_linear_page_index()):
 *
 *   VMA kind             vm_pgoff unit   vm_slice_off   PTE maps slice
 *   native (any)         native pages    0              -
 *   compat private anon  process pages   0              address bits
 *   compat file/shared   native pages    first slice    (addr - vm_start)/4K
 *                                                       + vm_slice_off
 *
 * MM_PAGE_*(mm) is the process geometry of @mm; mm == NULL means the
 * kernel's native geometry, so MM_PAGE_SIZE(NULL) == PAGE_SIZE and no
 * caller needs "mm ? MM_PAGE_SIZE(mm) : PAGE_SIZE".  MM_UAPI_*(mm) in
 * page_size_compat_defs.h is the same thing on PPPS kernels and falls back
 * to the x86 page-size-emulation geometry elsewhere; use it at syscall
 * entry points that must compile on both.
 */
#ifndef _LINUX_PPPS_H
#define _LINUX_PPPS_H

#include <asm/page.h>

#ifndef __ASSEMBLY__
#include <asm/current.h>

struct mm_struct;
struct linux_binprm;

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define PAGE_SHIFT_COMPAT	12
#define VA_BITS_COMPAT		39
#define _MM_PAGE_SHIFT_HELPER(mm) \
	((mm) && (mm)->page_shift ? (mm)->page_shift : PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm) \
	(((mm) && (mm)->page_shift == PAGE_SHIFT_COMPAT) ? VA_BITS_COMPAT : VA_BITS)
#define ppps_mm_is_compat(mm)						\
	((mm) && (mm)->page_shift == PAGE_SHIFT_COMPAT)

unsigned long mm_default_map_window64(void);
unsigned long mm_default_map_window64_of(struct mm_struct *mm);

void mm_init_pagesize(struct mm_struct *mm, const struct linux_binprm *bprm);

#else
#define PAGE_SHIFT_COMPAT	PAGE_SHIFT
#define VA_BITS_COMPAT		VA_BITS
#define PGTABLE_MM()		(NULL)
#define _MM_PAGE_SHIFT_HELPER(mm) \
	((void)(mm), PAGE_SHIFT)
#define _MM_VA_BITS_HELPER(mm)		((void)(mm), VA_BITS)
#define ppps_mm_is_compat(mm)		((void)(mm), false)

#define mm_default_map_window64()	(1UL << VA_BITS_MIN)
#define mm_default_map_window64_of(mm)	((void)(mm), (1UL << VA_BITS_MIN))

static inline void mm_init_pagesize(struct mm_struct *mm, const struct linux_binprm *bprm) {}
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

#define LEVEL_SHIFT_COMPAT	(PAGE_SHIFT_COMPAT - 3)

#define PMD_SHIFT_COMPAT	(PAGE_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)
#define PUD_SHIFT_COMPAT	(PMD_SHIFT_COMPAT + LEVEL_SHIFT_COMPAT)

#define PMD_SIZE_COMPAT		(1UL << PMD_SHIFT_COMPAT)
#define PUD_SIZE_COMPAT		(1UL << PUD_SHIFT_COMPAT)

#define MM_PAGE_SIZE(...)	(1UL << MM_PAGE_SHIFT(__VA_ARGS__))
#define MM_PAGE_MASK(...)	(~(MM_PAGE_SIZE(__VA_ARGS__) - 1))

#define MM_LEVEL_SHIFT(...)	(MM_PAGE_SHIFT(__VA_ARGS__) - 3)

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
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

/*
 * The mm whose page-table geometry applies to @addr: kernel addresses always
 * use the native geometry, which MM_*(NULL) yields.  Use as
 * MM_PMD_SHIFT(pgt_mm(addr, mm)) when a walk may cover both halves.
 */
#define pgt_mm(addr, mm)	(IS_KERNEL_ADDR(addr) ? NULL : (mm))

#endif /* __ASSEMBLY__ */

#endif /* _LINUX_PPPS_H */
