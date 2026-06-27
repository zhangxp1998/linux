// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/personality.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/binfmts.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/ppps.h>
#include <linux/highmem.h>
#include <asm/memory.h>
#include "internal.h"

#ifndef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#undef mm_task_size64
#undef mm_task_size64_of
#endif

unsigned long mm_task_size64(void)
{
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	struct mm_struct *mm = current->mm;

	if (!mm)
		return (1UL << vabits_actual);

	if (mm->page_shift == PAGE_SHIFT_COMPAT)
		return 1UL << VA_BITS_COMPAT;
#endif

	return 1UL << vabits_actual;
}
EXPORT_SYMBOL(mm_task_size64);

unsigned long mm_task_size64_of(struct mm_struct *mm)
{
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	if (!mm)
		return (1UL << vabits_actual);

	if (mm->page_shift == PAGE_SHIFT_COMPAT)
		return 1UL << VA_BITS_COMPAT;
#else
	(void)mm;
#endif

	return 1UL << vabits_actual;
}
EXPORT_SYMBOL(mm_task_size64_of);

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
unsigned long mm_default_map_window64(void)
{
	return mm_default_map_window64_of(current->mm);
}

unsigned long mm_default_map_window64_of(struct mm_struct *mm)
{
	if (ppps_mm_is_compat(mm))
		return 1UL << VA_BITS_COMPAT;

	return 1UL << VA_BITS_MIN;
}
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

#endif
