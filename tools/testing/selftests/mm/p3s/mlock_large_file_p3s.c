// SPDX-License-Identifier: GPL-2.0
/*
 * mlock() and MLOCK_ONFAULT on a 2MB private file mapping of a 4K compat
 * process accept the entire byte range and allow reads of every slice.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "p3s_test.h"
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)

static unsigned char slice_pattern(size_t offset)
{
	return ((offset / PROCESS_PAGE_SIZE) % 251) + 1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[PROCESS_PAGE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		memset(buffer, slice_pattern(offset), sizeof(buffer));
		if (pwrite(fd, buffer, sizeof(buffer), offset) !=
		    (ssize_t)sizeof(buffer))
			return -1;
	}
	if (fsync(fd))
		return -1;
	rc = posix_fadvise(fd, 0, MAP_SIZE, POSIX_FADV_DONTNEED);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

static int raise_memlock_limit(void)
{
	struct rlimit limit;

	if (getrlimit(RLIMIT_MEMLOCK, &limit))
		return -1;
	if (limit.rlim_cur >= MAP_SIZE)
		return 0;
	/* Raising the hard limit needs CAP_SYS_RESOURCE (EPERM otherwise). */
	if (limit.rlim_max < MAP_SIZE)
		limit.rlim_max = MAP_SIZE;
	limit.rlim_cur = MAP_SIZE;
	return setrlimit(RLIMIT_MEMLOCK, &limit);
}

static const char *test_file;

FIXTURE(large_file_lock) {
	char path[PATH_MAX];
	int fd;
	unsigned char *map;
	bool created_file;
};

FIXTURE_VARIANT(large_file_lock) {
	bool onfault;
};

FIXTURE_VARIANT_ADD(large_file_lock, mlock) { };
FIXTURE_VARIANT_ADD(large_file_lock, onfault) { .onfault = true };

FIXTURE_SETUP(large_file_lock)
{
	/* Create the file in the test body so parent teardown always runs. */
}

static int prepare_large_file(struct _test_data_large_file_lock *self,
			      const struct _fixture_variant_large_file_lock *variant)
{
	const char *prefix = variant->onfault ?
		"mlock-onfault-large-file-p3s.XXXXXX" :
		"mlock-large-file-p3s.XXXXXX";

	if (test_file) {
		if (snprintf(self->path, sizeof(self->path), "%s%s",
			     test_file, variant->onfault ? ".onfault" : "") >=
		    sizeof(self->path))
			return -1;
		self->fd = open(self->path,
				O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	} else {
		snprintf(self->path, sizeof(self->path), "%s", prefix);
		self->fd = mkstemp(self->path);
	}
	if (self->fd < 0)
		return -1;
	self->created_file = true;
	if (prepare_file(self->fd))
		return -1;
	self->map = mmap(MAP_ADDR, MAP_SIZE, PROT_READ,
			 MAP_PRIVATE | MAP_FIXED_NOREPLACE, self->fd, 0);
	return self->map == MAP_ADDR ? 0 : -1;
}

/* Remove the file even when an assertion aborts the test child. */
FIXTURE_TEARDOWN_PARENT(large_file_lock)
{
	if (self->created_file)
		EXPECT_EQ(0, unlink(self->path));
}

static int lock_and_fault(const struct _test_data_large_file_lock *self,
			  bool onfault, size_t *checksum)
{
	if (onfault && mlock2(self->map, MAP_SIZE, MLOCK_ONFAULT))
		return -1;
	*checksum = 0;
	for (size_t offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE)
		*checksum += self->map[offset];
	if (!onfault && mlock(self->map, MAP_SIZE))
		return -1;
	return 0;
}

TEST_F(large_file_lock, mapping_can_be_locked)
{
	size_t checksum;

	ASSERT_EQ(0, prepare_large_file(self, variant));
	if (raise_memlock_limit())
		SKIP(return, "RLIMIT_MEMLOCK is too small");
	ASSERT_EQ(0, lock_and_fault(self, variant->onfault, &checksum));
	EXPECT_GT(checksum, 0);
}

static void setup_harness(int *argc, char ***argv)
{
	test_file = p3s_harness_take_positional(argc, argv);
}

P3S_HARNESS_4KB_MAIN
