/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VDSO_DATASTORE_H
#define _LINUX_VDSO_DATASTORE_H

#include <linux/mm_types.h>

extern const struct vm_special_mapping vdso_vvar_mapping;
struct vm_area_struct *vdso_install_vvar_mapping(struct mm_struct *mm, unsigned long addr);
unsigned long arch_vdso_arch_data_pfn(struct vm_area_struct *vma,
				      unsigned long pgoff);

#endif /* _LINUX_VDSO_DATASTORE_H */
