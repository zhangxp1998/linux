// SPDX-License-Identifier: GPL-2.0
/*
 * mlock() and MLOCK_ONFAULT on a 2MB private file mapping of a 4K compat
 * process mark the backing large file folios mlocked and unevictable in
 * /proc/kpageflags.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)

#define P3S_KPF_UNEVICTABLE BIT_ULL(18)
#define P3S_KPF_MLOCKED BIT_ULL(33)

struct page_info {
	uint64_t pfn;
	uint64_t flags;
};

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

static int read_u64(int fd, uint64_t index, uint64_t *value)
{
	off_t offset = index * sizeof(*value);

	return pread(fd, value, sizeof(*value), offset) == sizeof(*value) ? 0 : -1;
}

static int collect_pages(const unsigned char *map, struct page_info *pages,
			 size_t *nr_pages)
{
	int flags_fd = -1;
	int pagemap_fd = -1;
	size_t offset;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0 || flags_fd < 0)
		goto error;

	*nr_pages = 0;
	for (offset = 0; offset < MAP_SIZE; offset += PROCESS_PAGE_SIZE) {
		uint64_t entry;
		uint64_t pfn;
		size_t i;

		if (read_u64(pagemap_fd,
			     (uintptr_t)(map + offset) / PROCESS_PAGE_SIZE, &entry))
			goto error;
		if (!(entry & PM_PRESENT)) {
			errno = ENOENT;
			goto error;
		}
		pfn = p3s_native_pfn(PAGEMAP_PFN(entry));
		if (!pfn) {
			errno = EPERM;
			goto error;
		}
		for (i = 0; i < *nr_pages; i++)
			if (pages[i].pfn == pfn)
				break;
		if (i != *nr_pages)
			continue;
		pages[i].pfn = pfn;
		if (read_u64(flags_fd, pfn, &pages[i].flags))
			goto error;
		(*nr_pages)++;
	}

	close(flags_fd);
	close(pagemap_fd);
	return 0;

error:
	if (flags_fd >= 0)
		close(flags_fd);
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	return -1;
}

static size_t count_locked_large_pages(const struct page_info *pages,
				       size_t nr_pages, size_t *large_pages)
{
	size_t locked = 0;
	size_t i;

	*large_pages = 0;
	for (i = 0; i < nr_pages; i++) {
		uint64_t flags = pages[i].flags;

		if (!(flags & (KPF_COMPOUND_HEAD | KPF_COMPOUND_TAIL)))
			continue;
		(*large_pages)++;
		if ((flags & P3S_KPF_MLOCKED) &&
		    (flags & P3S_KPF_UNEVICTABLE))
			locked++;
	}
	return locked;
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

TEST_F(large_file_lock, large_folio_becomes_unevictable)
{
	struct page_info pages[MAP_SIZE / PROCESS_PAGE_SIZE];
	size_t checksum, nr_pages, large_pages, locked_large;

	ASSERT_EQ(0, prepare_large_file(self, variant));
	if (raise_memlock_limit())
		SKIP(return, "RLIMIT_MEMLOCK is too small");
	ASSERT_EQ(0, lock_and_fault(self, variant->onfault, &checksum));
	ASSERT_GT(checksum, 0);
	if (collect_pages(self->map, pages, &nr_pages))
		SKIP(return, "PFNs or kpageflags are unavailable");
	locked_large = count_locked_large_pages(pages, nr_pages, &large_pages);
	TH_LOG("unique=%zu large=%zu locked_large=%zu", nr_pages,
	       large_pages, locked_large);
	if (!large_pages)
		SKIP(return, "filesystem did not allocate a large folio");
	EXPECT_GT(locked_large, 0);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode) {
		if (argc > 2)
			return EXIT_FAILURE;
		if (!p3s_is_compat_process()) {
			if (argc == 2)
				exec_compat(argv[0], P3S_RUN_FLAG, argv[1], NULL);
			exec_compat(argv[0], P3S_RUN_FLAG, NULL);
		}
		test_file = argc == 2 ? argv[1] : NULL;
		return p3s_harness_run();
	}
	if ((argc == 2 || argc == 3) && !strcmp(mode, P3S_RUN_FLAG)) {
		p3s_require_compat();
		test_file = argc == 3 ? argv[2] : NULL;
		return p3s_harness_run();
	}
	return EXIT_FAILURE;
}
