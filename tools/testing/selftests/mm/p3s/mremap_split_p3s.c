// SPDX-License-Identifier: GPL-2.0-only
/* A 4KB process may shrink or retain a range split into 4KB VMAs by mprotect. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "p3s_harness.h"

static bool check_split(bool shrink, bool *retained)
{
	size_t page = sysconf(_SC_PAGESIZE);
	size_t old_len = 4 * page, new_len = shrink ? page : old_len;
	unsigned char *base, *result;
	unsigned char vec;
	bool ok;

	base = mmap(NULL, old_len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return false;
	memset(base, 0x5a, old_len);
	if (mprotect(base + page, page, PROT_READ)) {
		munmap(base, old_len);
		return false;
	}
	result = mremap(base, old_len, new_len, 0);
	ok = result == base;
	if (ok && shrink)
		*retained = mincore(base + page, page, &vec) == -1 &&
			    errno == ENOMEM;
	else if (ok)
		*retained = base[3 * page] == 0x5a;
	if (ok)
		*retained &= base[0] == 0x5a && base[page - 1] == 0x5a;
	munmap(base, result == MAP_FAILED ? old_len : new_len);
	return ok;
}

TEST(no_op_mremap_crosses_split_vma)
{
	bool retained = false;

	ASSERT_TRUE(check_split(false, &retained));
	EXPECT_TRUE(retained);
}

TEST(shrink_mremap_crosses_split_vma)
{
	bool retained = false;

	ASSERT_TRUE(check_split(true, &retained));
	EXPECT_TRUE(retained);
}

P3S_HARNESS_4KB_MAIN
