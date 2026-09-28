// SPDX-License-Identifier: GPL-2.0-only
/* A private COW/unmap cycle must not accumulate references to its file page. */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "p3s_test.h"
#include "p3s_harness.h"
#include "iov_iter_p3s.h"
#include "page_range_p3s.h"

static int file_refs(int fixture, int file)
{
	struct file_refs_p3s_args request = { .fd = file };

	if (ioctl(fixture, FILE_REFS_P3S_IOCTL, &request))
		return -1;
	return request.refs;
}

FIXTURE(file_cow_refs) {
	int fixture;
	int file;
};

FIXTURE_SETUP(file_cow_refs)
{
	unsigned char initial[NATIVE_PAGE_SIZE];

	if (access("/dev/" IOV_ITER_P3S_DEVICE_NAME, R_OK | W_OK))
		SKIP(return, "iov_iter fixture is unavailable");
	self->fixture = open("/dev/" IOV_ITER_P3S_DEVICE_NAME, O_RDWR);
	ASSERT_GE(self->fixture, 0);
	self->file = memfd_create("file-cow-references", MFD_CLOEXEC);
	ASSERT_GE(self->file, 0);
	memset(initial, 0x37, sizeof(initial));
	ASSERT_EQ(sizeof(initial), write(self->file, initial, sizeof(initial)));
}

FIXTURE_TEARDOWN(file_cow_refs)
{
	close(self->file);
	close(self->fixture);
}

static bool run_cycles(FIXTURE_DATA(file_cow_refs) *self, bool *contents,
		       int *before, int *after)
{
	size_t page = sysconf(_SC_PAGESIZE);
	int round;
	size_t i;

	*contents = true;
	*before = file_refs(self->fixture, self->file);
	if (*before < 0)
		return false;
	for (round = 0; round < 8; round++) {
		volatile unsigned char *map = mmap(NULL, NATIVE_PAGE_SIZE,
			PROT_READ | PROT_WRITE, MAP_PRIVATE, self->file, 0);

		if ((void *)map == MAP_FAILED)
			return false;
		/* Populate file PTEs before any writes, then COW each slice. */
		for (i = 0; i < NATIVE_PAGE_SIZE; i += page)
			*contents &= map[i] == 0x37;
		for (i = 0; i < NATIVE_PAGE_SIZE; i += page)
			map[i] = 0x80 + round;
		for (i = 0; i < NATIVE_PAGE_SIZE; i += page)
			*contents &= map[i] == 0x80 + round;
		if (munmap((void *)map, NATIVE_PAGE_SIZE))
			return false;
	}
	*after = file_refs(self->fixture, self->file);
	return *after >= 0;
}

TEST_F(file_cow_refs, repeated_cow_preserves_private_bytes)
{
	bool contents;
	int before, after;

	ASSERT_TRUE(run_cycles(self, &contents, &before, &after));
	EXPECT_TRUE(contents);
}

TEST_F(file_cow_refs, completed_cow_does_not_accumulate_file_refs)
{
	bool contents;
	int before, after;

	ASSERT_TRUE(run_cycles(self, &contents, &before, &after));
	/* One transient LRU-vector reference may differ between observations. */
	EXPECT_LE(after, before + 1);
}

P3S_HARNESS_4KB_MAIN
