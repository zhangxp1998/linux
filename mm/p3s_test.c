// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic MMs exercise context selection, not live user page tables. */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include <linux/p3s_user_pages.h>

struct p3s_test_mms {
	struct mm_struct *native;
	struct mm_struct *compat;
};

static int p3s_test_init(struct kunit *test)
{
	struct p3s_test_mms *mms;

	mms = kunit_kzalloc(test, sizeof(*mms), GFP_KERNEL);
	if (!mms)
		return -ENOMEM;
	mms->native = kunit_kzalloc(test, sizeof(*mms->native), GFP_KERNEL);
	mms->compat = kunit_kzalloc(test, sizeof(*mms->compat), GFP_KERNEL);
	if (!mms->native || !mms->compat)
		return -ENOMEM;
	mms->native->page_shift = PAGE_SHIFT_KERNEL;
	mms->compat->page_shift = PAGE_SHIFT_COMPAT;
	test->priv = mms;
	return 0;
}

static void p3s_nested_contexts(struct kunit *test)
{
	struct p3s_test_mms *mms = test->priv;
	struct mm_struct *saved = current->p3s_remote_mm;

	{
		P3S_CONTEXT_REMOTE_MM(mms->compat);

		KUNIT_EXPECT_EQ(test, PAGE_SIZE, PAGE_SIZE_COMPAT);
		KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->compat);
		{
			P3S_CONTEXT_REMOTE_MM(mms->native);

			KUNIT_EXPECT_EQ(test, PAGE_SIZE, PAGE_SIZE_KERNEL);
			KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->native);
		}
		KUNIT_EXPECT_EQ(test, PAGE_SIZE, PAGE_SIZE_COMPAT);
		{
			P3S_CONTEXT_REMOTE_MM(current->mm);

			KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), current->mm);
		}
		KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->compat);
	}
	KUNIT_EXPECT_PTR_EQ(test, current->p3s_remote_mm, saved);
}

static unsigned long p3s_return_from_scope(struct mm_struct *mm)
{
	P3S_CONTEXT_REMOTE_MM(mm);

	return PAGE_SIZE;
}

static unsigned long p3s_goto_from_scope(struct mm_struct *mm)
{
	unsigned long size;

	{
		P3S_CONTEXT_REMOTE_MM(mm);

		size = PAGE_SIZE;
		goto out;
	}
out:
	return size;
}

static void p3s_early_exit_restores(struct kunit *test)
{
	struct p3s_test_mms *mms = test->priv;

	P3S_CONTEXT_REMOTE_MM(mms->native);

	KUNIT_EXPECT_EQ(test, p3s_return_from_scope(mms->compat), PAGE_SIZE_COMPAT);
	KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->native);
	KUNIT_EXPECT_EQ(test, p3s_goto_from_scope(mms->compat), PAGE_SIZE_COMPAT);
	KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->native);
}

static void p3s_native_units_stay_native(struct kunit *test)
{
	struct p3s_test_mms *mms = test->priv;
	unsigned long pfn = 0x1234;

	P3S_CONTEXT_REMOTE_MM(mms->compat);

	KUNIT_EXPECT_EQ(test, MM_PAGE_SIZE(NULL), PAGE_SIZE_KERNEL);
	KUNIT_EXPECT_EQ(test, PPPS_SLICES_PER_PAGE, 4UL);
	KUNIT_EXPECT_EQ(test, PAGE_ALIGN(1), PAGE_SIZE_COMPAT);
	KUNIT_EXPECT_EQ(test, PAGE_ALIGN_KERNEL(1), PAGE_SIZE_KERNEL);
	KUNIT_EXPECT_EQ(test, PFN_UP(PAGE_SIZE_COMPAT), 1UL);
	KUNIT_EXPECT_EQ(test, PFN_PHYS(pfn), (phys_addr_t)pfn << PAGE_SHIFT_KERNEL);
	KUNIT_EXPECT_EQ(test, PHYS_PFN(PFN_PHYS(pfn)), pfn);
	KUNIT_EXPECT_EQ(test, pte_pfn(pfn_pte(pfn, PAGE_NONE)), pfn);
	KUNIT_EXPECT_EQ(test, HPAGE_PMD_ORDER, HPAGE_PMD_SHIFT - PAGE_SHIFT_KERNEL);
	KUNIT_EXPECT_EQ(test, SWP_PFN_BITS, MAX_PHYSMEM_BITS - PAGE_SHIFT_KERNEL);
}

struct p3s_child_check {
	struct completion done;
	bool cleared;
};

static int p3s_child_context(void *arg)
{
	struct p3s_child_check *check = arg;

	check->cleared = !current->p3s_remote_mm;
	complete(&check->done);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void p3s_async_context_is_independent(struct kunit *test)
{
	struct p3s_test_mms *mms = test->priv;
	struct p3s_child_check check;
	struct task_struct *child;

	P3S_CONTEXT_REMOTE_MM(mms->compat);

	init_completion(&check.done);
	check.cleared = false;
	child = kthread_run(p3s_child_context, &check, "p3s-kunit");
	KUNIT_EXPECT_FALSE(test, IS_ERR(child));
	if (IS_ERR(child))
		return;
	wait_for_completion(&check.done);
	kthread_stop(child);
	KUNIT_EXPECT_TRUE(test, check.cleared);
	KUNIT_EXPECT_PTR_EQ(test, p3s_current_mm(), mms->compat);
}

static struct kunit_case p3s_cases[] = {
	KUNIT_CASE(p3s_nested_contexts),
	KUNIT_CASE(p3s_early_exit_restores),
	KUNIT_CASE(p3s_native_units_stay_native),
	KUNIT_CASE(p3s_async_context_is_independent),
	{}
};

static struct kunit_suite p3s_suite = {
	.name = "ppps-page-context",
	.init = p3s_test_init,
	.test_cases = p3s_cases,
};

kunit_test_suite(p3s_suite);

MODULE_LICENSE("GPL");
