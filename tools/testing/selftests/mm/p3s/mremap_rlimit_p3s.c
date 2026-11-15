// SPDX-License-Identifier: GPL-2.0
/*
 * With RLIMIT_AS pinned to a 4K compat process's current statm size, a
 * one-process-page mremap expansion is rejected with ENOMEM, and succeeds
 * when the limit allows exactly one more 4KB page.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>

#include "p3s_harness.h"

static bool current_vm_pages(unsigned long *pages)
{
	FILE *statm = fopen("/proc/self/statm", "r");
	bool read_ok;

	if (!statm)
		return false;
	read_ok = fscanf(statm, "%lu", pages) == 1;
	fclose(statm);
	return read_ok;
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
	ASSERT_TRUE(current_vm_pages(&vm_pages));
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
	if (result != MAP_FAILED)
		munmap(result, 2 * PROCESS_PAGE_SIZE);
	ASSERT_TRUE(rejected);

	limit.rlim_cur = (vm_pages + 1) * PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, setrlimit(RLIMIT_AS, &limit));
	result = mremap(mapping, PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
			MREMAP_MAYMOVE);
	ASSERT_EQ(0, setrlimit(RLIMIT_AS, &saved));
	EXPECT_NE(MAP_FAILED, result);
	if (result != MAP_FAILED)
		munmap(result, 2 * PROCESS_PAGE_SIZE);
	else
		munmap(mapping, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
