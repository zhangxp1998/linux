/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_SHMPARAM_H
#define __ASM_SHMPARAM_H

#include <linux/ppps.h>

/*
 * For AArch32 IPC syscalls retain four process pages of alignment. Since we don't have aliasing D-caches, native tasks
 * can use their process page size. Keep SHMLBA at the kernel page size
 * for kernel mappings which use it as a cache-aliasing constraint.
 */
#define SHMLBA_USER	MM_PAGE_SIZE(current->mm)
#define COMPAT_SHMLBA	(4 * MM_PAGE_SIZE(current->mm))

#include <asm-generic/shmparam.h>

#endif /* __ASM_SHMPARAM_H */
