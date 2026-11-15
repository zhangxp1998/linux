// SPDX-License-Identifier: GPL-2.0
/*
 * With RLIMIT_AS pinned to a 4K compat process's current statm size, a
 * one-process-page mremap expansion is rejected with ENOMEM.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>

#include "p3s_harness.h"

static unsigned long current_vm_pages(void)
{
	unsigned long pages;
	FILE *statm = fopen("/proc/self/statm", "r");

	if (!statm)
		ksft_exit_fail_msg("open statm failed: %s\n", strerror(errno));
	if (fscanf(statm, "%lu", &pages) != 1)
		ksft_exit_fail_msg("read statm failed\n");
	fclose(statm);
	return pages;
}

TEST(expansion_respects_rlimit_as)
{
	struct rlimit limit;
	struct rlimit saved;
	unsigned long vm_pages;
	void *mapping;
	void *result;
	bool rejected;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	vm_pages = current_vm_pages();
	/*
	 * Lower only the soft limit: it is restored before the test prints
	 * or exits, both of which need to allocate.
	 */
	ASSERT_EQ(0, getrlimit(RLIMIT_AS, &saved));
	limit.rlim_cur = vm_pages * PROCESS_PAGE_SIZE;
	limit.rlim_max = saved.rlim_max;
	ASSERT_EQ(0, setrlimit(RLIMIT_AS, &limit));

	errno = 0;
	result = mremap(mapping, PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
			MREMAP_MAYMOVE);
	rejected = result == MAP_FAILED && errno == ENOMEM;
	ASSERT_EQ(0, setrlimit(RLIMIT_AS, &saved));
	EXPECT_TRUE(rejected);
	TH_LOG("statm_pages=%lu limit=%llu mremap=%p errno=%d",
		       vm_pages, (unsigned long long)limit.rlim_cur, result, errno);

	if (result != MAP_FAILED)
		munmap(result, 2 * PROCESS_PAGE_SIZE);
	else
		munmap(mapping, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_COMPAT_MAIN
