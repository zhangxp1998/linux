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

#endif /* _LINUX_PPPS_H */
