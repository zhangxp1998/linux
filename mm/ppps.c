// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/personality.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/binfmts.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/ppps.h>
#include <linux/string.h>
#include <asm/memory.h>

#include "ppps.h"

unsigned long mm_task_size64_of(struct mm_struct *mm)
{
	return ppps_mm_is_compat(mm) ? 1UL << VA_BITS_COMPAT :
				       1UL << vabits_actual;
}
EXPORT_SYMBOL(mm_task_size64_of);

unsigned long mm_task_size64(void)
{
	return mm_task_size64_of(current->mm);
}
EXPORT_SYMBOL(mm_task_size64);

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
unsigned long mm_default_map_window64_of(struct mm_struct *mm)
{
	return ppps_mm_is_compat(mm) ? 1UL << VA_BITS_COMPAT :
				       1UL << VA_BITS_MIN;
}
EXPORT_SYMBOL(mm_default_map_window64_of);

unsigned long mm_default_map_window64(void)
{
	return mm_default_map_window64_of(current->mm);
}
EXPORT_SYMBOL(mm_default_map_window64);

/* Testing only: select Android app runtimes, not init or its other services. */
static bool ppps_test_app_runtime(const struct linux_binprm *bprm)
{
	struct name_snapshot snapshot;
	bool match;

	take_dentry_name_snapshot(&snapshot, bprm->file->f_path.dentry);
	match = !strcmp(snapshot.name.name, "app_process") ||
		!strcmp(snapshot.name.name, "app_process32") ||
		!strcmp(snapshot.name.name, "app_process64") ||
		!strcmp(snapshot.name.name, "zygote") ||
		!strcmp(snapshot.name.name, "zygote32") ||
		!strcmp(snapshot.name.name, "zygote64");
	release_dentry_name_snapshot(&snapshot);
	return match;
}

void mm_init_pagesize(struct mm_struct *mm, const struct linux_binprm *bprm)
{
	if ((current->personality & ADDR_4KB_COMPAT_PAGE_SIZE) ||
	    ppps_test_app_runtime(bprm))
		mm->page_shift = PAGE_SHIFT_COMPAT;
	else
		mm->page_shift = PAGE_SHIFT;
}

/*
 * vm_insert_pages() for a compat VMA: every slice of each native page in
 * @pages is mapped by one process-page PTE, consecutively from @addr up to
 * the end of the VMA.  On return *num is the number of pages that were not
 * (completely) mapped, like vm_insert_pages().
 */
int ppps_vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			 struct page **pages, unsigned long *num)
{
	const unsigned long nr_pages = *num;
	unsigned long user_pages, logical_pages, i;

	if (!nr_pages)
		return 0;
	if (addr < vma->vm_start || addr >= vma->vm_end)
		return -EFAULT;

	/* pages[] always starts at byte zero, independently of the VMA offset. */
	if (!MM_PAGE_ALIGNED(vma->vm_mm, addr))
		return -EFAULT;
	user_pages = (vma->vm_end - addr) >> PAGE_SHIFT_COMPAT;
	if (nr_pages > DIV_ROUND_UP(user_pages, PPPS_SLICES_PER_PAGE))
		return -EFAULT;
	/* The final native page may cover only the remaining VMA slices. */
	logical_pages = min(user_pages, nr_pages << PPPS_SLICE_SHIFT);
	for (i = 0; i < logical_pages; i++) {
		unsigned long total_slice = i;
		unsigned long page_index = total_slice >> PPPS_SLICE_SHIFT;
		int error;

		error = vm_insert_page_slice(vma, addr, pages[page_index],
					     total_slice & PPPS_SLICE_MASK);
		if (error) {
			*num = nr_pages - page_index;
			return error;
		}
		addr += PAGE_SIZE_COMPAT;
	}
	*num = 0;
	return 0;
}
#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */
