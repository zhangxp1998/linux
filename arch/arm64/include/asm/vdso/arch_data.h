/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_VDSO_ARCH_DATA_H
#define __ASM_VDSO_ARCH_DATA_H

#include <linux/types.h>

struct vdso_arch_data {
	__u32 page_shift;
};

#endif /* __ASM_VDSO_ARCH_DATA_H */
