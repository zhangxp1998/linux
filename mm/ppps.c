// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/personality.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/ppps.h>
#include <asm/memory.h>

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

unsigned long mm_default_map_window64(void)
{
	return mm_default_map_window64_of(current->mm);
}
#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */
