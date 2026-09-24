/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_USER_PAGES_H
#define _LINUX_P3S_USER_PAGES_H

/*
 * Explicit opt-in to task-context user-page macros. Include LAST in a .c
 * file, never from a public header. All following PAGE_* uses need a unit
 * audit: physical pages remain PAGE_*_KERNEL; independently owned MMs retain
 * explicit MM_PAGE_* helpers. Inline bodies in earlier headers stay native,
 * but macros expand at the use site, so native derived macros must be fixed
 * to PAGE_*_KERNEL as well.
 *
 * Keep page-table walks and address-space limits on our explicit MM helpers.
 * Selecting user arithmetic must not change native PFNs or allocator units.
 */
#include <linux/mm.h>
#include <linux/p3s.h>
#include <linux/page_size_compat.h>

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#undef PAGE_SHIFT
#undef PAGE_SIZE
#undef PAGE_MASK
#define PAGE_SHIFT	MM_PAGE_SHIFT(p3s_current_mm())
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE - 1))

/* Match the PPPS MM_UAPI_* contract; emulation diagnostics are x86-only. */
#undef __PAGE_ALIGNED
#undef __offset_in_page_log
#define __PAGE_ALIGNED(addr)	PAGE_ALIGNED(addr)
#define __offset_in_page_log(addr)	offset_in_page(addr)

/* Physical PFNs always use the kernel base-page unit. */
#undef PFN_ALIGN
#undef PFN_UP
#undef PFN_DOWN
#undef PFN_PHYS
#undef PHYS_PFN
#define PFN_ALIGN(x)	PAGE_ALIGN_KERNEL((unsigned long)(x))
#define PFN_UP(x)	(((x) + PAGE_SIZE_KERNEL - 1) >> PAGE_SHIFT_KERNEL)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT_KERNEL)
#define PFN_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT_KERNEL)
#define PHYS_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT_KERNEL))
#endif

#endif /* _LINUX_P3S_USER_PAGES_H */
