// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/ppps.h>

#include <asm/pgtable.h>

#define TEST_PTE_PROT	(PTE_TYPE_PAGE | PTE_AF | PTE_USER | PTE_RDONLY | \
			 PTE_NG | PTE_DBM | PTE_PXN | PTE_UXN | PTE_DIRTY | \
			 PTE_SPECIAL)

static pte_t pte_advance_test_entry(phys_addr_t phys)
{
	return __pte(__phys_to_pte_val(phys) | TEST_PTE_PROT);
}

static pteval_t pte_advance_test_non_phys(pte_t pte)
{
	phys_addr_t phys = __pte_to_phys(pte);

	return pte_val(pte) ^ __phys_to_pte_val(phys);
}

static void pte_advance_test_sequence(struct kunit *test,
				      unsigned long stride,
				      unsigned int nr_steps)
{
	const phys_addr_t base = SZ_1G;
	pte_t pte = pte_advance_test_entry(base);
	unsigned int i;

	for (i = 0; i < nr_steps; i++) {
		phys_addr_t expected = base + (phys_addr_t)i * stride;

		KUNIT_EXPECT_EQ_MSG(test, __pte_to_phys(pte), expected,
				    "step %u, stride %#lx", i, stride);
		KUNIT_EXPECT_EQ_MSG(test, pte_advance_test_non_phys(pte),
				    (pteval_t)TEST_PTE_PROT,
				    "attributes changed at step %u", i);
		pte = pte_advance_phys(pte, stride);
	}
}

static void pte_advance_phys_native_test(struct kunit *test)
{
	pte_advance_test_sequence(test, PAGE_SIZE, 8);
}

#if IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE)
static void pte_advance_phys_compat_test(struct kunit *test)
{
	KUNIT_ASSERT_LT(test, PAGE_SIZE_COMPAT, PAGE_SIZE);
	pte_advance_test_sequence(test, PAGE_SIZE_COMPAT, 8);
}
#endif

static struct kunit_case pte_test_cases[] = {
	KUNIT_CASE(pte_advance_phys_native_test),
#if IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE)
	KUNIT_CASE(pte_advance_phys_compat_test),
#endif
	{}
};

static struct kunit_suite pte_test_suite = {
	.name = "arm64-pte",
	.test_cases = pte_test_cases,
};

kunit_test_suite(pte_test_suite);

MODULE_DESCRIPTION("KUnit tests for arm64 PTE helpers");
MODULE_LICENSE("GPL");
