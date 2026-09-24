/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_CONST_H
#define _LINUX_P3S_CONST_H

#include <asm/page.h>

/* Only arm64 PPPS redefines PAGE_*; elsewhere these are native aliases. */
#ifndef PAGE_SHIFT_KERNEL
#define PAGE_SHIFT_KERNEL	PAGE_SHIFT
#define PAGE_SIZE_KERNEL		PAGE_SIZE
#define PAGE_MASK_KERNEL		PAGE_MASK
#endif

#ifndef __ASSEMBLY__
#include <linux/align.h>

#define PAGE_ALIGN_KERNEL(addr)		ALIGN((addr), PAGE_SIZE_KERNEL)
#define PAGE_ALIGN_DOWN_KERNEL(addr)	ALIGN_DOWN((addr), PAGE_SIZE_KERNEL)
#define PAGE_ALIGNED_KERNEL(addr)		IS_ALIGNED((unsigned long)(addr), PAGE_SIZE_KERNEL)
#define offset_in_page_kernel(addr)	((unsigned long)(addr) & ~PAGE_MASK_KERNEL)
#endif

#endif /* _LINUX_P3S_CONST_H */
