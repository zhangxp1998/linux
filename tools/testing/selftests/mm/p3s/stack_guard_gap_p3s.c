// SPDX-License-Identifier: GPL-2.0
/*
 * The default 256-process-page stack guard gap accepts a hint ending at its
 * lower edge but rejects a hint one 4KB page inside it.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define GUARD_GAP_4K	(256 * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(GUARD_GAP_4K + 2 * PROCESS_PAGE_SIZE)

TEST(p3s_stack_guard_gap)
{
	unsigned char *reservation;
	unsigned char *stack_addr;
	void *mapping, *inside;
	void *stack;

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	stack_addr = reservation + GUARD_GAP_4K + PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, munmap(reservation, RESERVE_SIZE));

	stack = mmap(stack_addr, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE |
		     MAP_GROWSDOWN, -1, 0);
	ASSERT_EQ(stack_addr, stack);

	mapping = mmap(reservation, PROCESS_PAGE_SIZE, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_EQ(reservation, mapping);
	/* The next hint starts one process page inside the guard gap. */
	inside = mmap(stack_addr - GUARD_GAP_4K, PROCESS_PAGE_SIZE,
		      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(inside, MAP_FAILED);
	EXPECT_TRUE((unsigned char *)inside + PROCESS_PAGE_SIZE <=
		    stack_addr - GUARD_GAP_4K ||
		    (unsigned char *)inside >= stack_addr + PROCESS_PAGE_SIZE);
	if (inside != MAP_FAILED)
		munmap(inside, PROCESS_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	munmap(stack, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
