// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#define SLICE_SIZE 4096UL
#define MAP_SIZE (2 * 1024 * 1024UL)
#define MAP_ADDR ((void *)0x40001000UL)

#define PAGEMAP_PRESENT 0x8000000000000000ULL
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define KPF_COMPOUND_HEAD 15
#define KPF_COMPOUND_TAIL 16
#define KPF_UNEVICTABLE 18
#define KPF_MLOCKED 33

struct page_info {
	uint64_t pfn;
	uint64_t flags;
};

static int failures;
static int test_no;

static void result(int pass, const char *name)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++test_no, name);
	if (!pass)
		failures++;
}

static void skip(const char *name, const char *reason)
{
	printf("ok %d - %s # SKIP %s\n", ++test_no, name, reason);
}

static unsigned char slice_pattern(size_t offset)
{
	return ((offset / SLICE_SIZE) % 251) + 1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[SLICE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < MAP_SIZE; offset += SLICE_SIZE) {
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
	if (limit.rlim_max < MAP_SIZE) {
		errno = EPERM;
		return -1;
	}
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
	for (offset = 0; offset < MAP_SIZE; offset += SLICE_SIZE) {
		uint64_t entry;
		uint64_t pfn;
		size_t i;

		if (read_u64(pagemap_fd,
			     (uintptr_t)(map + offset) / SLICE_SIZE, &entry))
			goto error;
		if (!(entry & PAGEMAP_PRESENT)) {
			errno = ENOENT;
			goto error;
		}
		pfn = entry & PAGEMAP_PFN_MASK;
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

		if (!(flags & ((1ULL << KPF_COMPOUND_HEAD) |
			       (1ULL << KPF_COMPOUND_TAIL))))
			continue;
		(*large_pages)++;
		if ((flags & (1ULL << KPF_MLOCKED)) &&
		    (flags & (1ULL << KPF_UNEVICTABLE)))
			locked++;
	}
	return locked;
}

int main(int argc, char **argv)
{
	struct page_info pages[MAP_SIZE / SLICE_SIZE];
	char path[PATH_MAX] = "mlock-large-file-ppps.XXXXXX";
	size_t checksum = 0;
	unsigned char *map = MAP_FAILED;
	size_t large_pages;
	size_t locked_large;
	size_t nr_pages;
	size_t offset;
	int flags_available;
	int limit_available;
	int fd = -1;
	int rc;

	printf("TAP version 13\n1..4\n");
	if (argc > 2) {
		printf("Bail out! usage: %s [FILE]\n", argv[0]);
		return 1;
	}
	result(sysconf(_SC_PAGESIZE) == SLICE_SIZE,
	       "process page size is 4K");

	if (argc == 2) {
		strncpy(path, argv[1], sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';
		fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	} else {
		fd = mkstemp(path);
	}
	rc = fd < 0 ? -1 : prepare_file(fd);
	if (rc)
		perror("prepare patterned file");
	if (!rc)
		map = mmap(MAP_ADDR, MAP_SIZE, PROT_READ,
			   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (map == MAP_FAILED && !rc) {
		perror("mmap");
		rc = -1;
	}
	if (!rc) {
		for (offset = 0; offset < MAP_SIZE; offset += SLICE_SIZE)
			checksum += map[offset];
	}
	printf("# mapped checksum=%zu\n", (size_t)checksum);
	result(!rc && map == MAP_ADDR, "map and fault a large file");
	if (rc)
		goto skip_remaining;

	limit_available = raise_memlock_limit() == 0;
	rc = limit_available ? mlock(map, MAP_SIZE) : -1;
	if (rc && limit_available)
		perror("mlock");
	if (!limit_available)
		skip("mlock the file mapping", "RLIMIT_MEMLOCK is too small");
	else
		result(!rc, "mlock the file mapping");

	flags_available = !rc && !collect_pages(map, pages, &nr_pages);
	if (!flags_available) {
		skip("mlock marks a large file folio unevictable",
		     "PFNs or kpageflags are unavailable");
	} else {
		locked_large = count_locked_large_pages(pages, nr_pages, &large_pages);
		printf("# unique=%zu large=%zu locked_large=%zu\n",
		       nr_pages, large_pages, locked_large);
		if (!large_pages)
			skip("mlock marks a large file folio unevictable",
			     "filesystem did not allocate a large folio");
		else
			result(locked_large > 0,
			       "mlock marks a large file folio unevictable");
	}
	goto out;

skip_remaining:
	skip("mlock the file mapping", "file mapping is unavailable");
	skip("mlock marks a large file folio unevictable",
	     "file mapping is unavailable");
out:
	if (map != MAP_FAILED) {
		munlock(map, MAP_SIZE);
		munmap(map, MAP_SIZE);
	}
	if (fd >= 0)
		close(fd);
	unlink(path);
	printf("# Totals: pass:%d fail:%d\n", test_no - failures, failures);
	return failures ? 1 : 0;
}
