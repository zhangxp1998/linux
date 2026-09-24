/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/cleanup.h>
#include <linux/mm_types.h>
#include <linux/ppps.h>
#include <linux/sched.h>

/*
 * Scoped MM selection, based on Kalesh Singh's P3S design. This header does
 * not override PAGE_*; that requires an explicit p3s_user_pages.h include.
 *
 * The scope borrows @mm, without taking a reference or switching page tables.
 * Its caller must keep @mm alive until scope exit. Synchronous callees see
 * the selection; asynchronous work must establish its own context. Entry
 * points operating on current's memory must explicitly select current->mm
 * rather than accidentally inheriting a remote caller's selection.
 */
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
static __always_inline struct mm_struct *p3s_current_mm(void)
{
	return current->p3s_remote_mm ?: current->mm;
}

static inline struct mm_struct *p3s_enter_remote_mm(struct mm_struct *mm)
{
	struct mm_struct *prev = current->p3s_remote_mm;

	current->p3s_remote_mm = mm;
	return prev;
}

static inline void p3s_exit_remote_mm(struct mm_struct *prev)
{
	current->p3s_remote_mm = prev;
}

DEFINE_CLASS(p3s_remote_mm, struct mm_struct *,
	     p3s_exit_remote_mm(_T), p3s_enter_remote_mm(_mm),
	     struct mm_struct *_mm);

#define P3S_CONTEXT_REMOTE_MM(mm) \
	CLASS(p3s_remote_mm, __UNIQUE_ID(p3s_rmm))(mm)
#else
static inline struct mm_struct *p3s_current_mm(void)
{
	return current->mm;
}

#define P3S_CONTEXT_REMOTE_MM(mm)		((void)(mm))
#endif

#endif /* _LINUX_P3S_H */
