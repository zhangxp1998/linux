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
#include <sys/stat.h>
#include <unistd.h>

#define SLICE_SIZE 4096UL
#define FILE_SIZE (2UL * 1024 * 1024)
#define MAP_ADDR ((void *)0x40001000UL)
#define PAGEMAP_PRESENT 0x8000000000000000ULL
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define KPF_COMPOUND_HEAD 15
#define KPF_COMPOUND_TAIL 16
#define KPF_UNEVICTABLE 18
#define KPF_MLOCKED 33
#define KSFT_SKIP 4

struct large_folio {
	size_t file_offset;
	size_t native_pages;
	size_t slices_per_native;
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

static int read_u64(int fd, uint64_t index, uint64_t *value)
{
	return pread(fd, value, sizeof(*value), index * sizeof(*value)) ==
	       sizeof(*value) ? 0 : -1;
}

static int virtual_pfn(int pagemap_fd, const void *address, uint64_t *pfn)
{
	uint64_t entry;

	if (read_u64(pagemap_fd, (uintptr_t)address / SLICE_SIZE, &entry) ||
	    !(entry & PAGEMAP_PRESENT))
		return -1;
	*pfn = entry & PAGEMAP_PFN_MASK;
	return *pfn ? 0 : -1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[SLICE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < FILE_SIZE; offset += sizeof(buffer)) {
		memset(buffer, (offset / SLICE_SIZE) % 251 + 1, sizeof(buffer));
		if (pwrite(fd, buffer, sizeof(buffer), offset) != sizeof(buffer))
			return -1;
	}
	if (fsync(fd))
		return -1;
	rc = posix_fadvise(fd, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

static int find_large_folio(unsigned char *map, struct large_folio *info)
{
	int pagemap_fd = -1;
	int flags_fd = -1;
	size_t offset;
	int rc = -1;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0 || flags_fd < 0)
		goto out;

	for (offset = 0; offset < FILE_SIZE; offset += SLICE_SIZE) {
		uint64_t flags;
		uint64_t pfn;
		size_t native_pages = 1;
		size_t slices = 1;

		if (virtual_pfn(pagemap_fd, map + offset, &pfn) ||
		    read_u64(flags_fd, pfn, &flags))
			goto out;
		if (!(flags & (1ULL << KPF_COMPOUND_HEAD)))
			continue;

		while (native_pages < 512) {
			if (read_u64(flags_fd, pfn + native_pages, &flags))
				goto out;
			if (!(flags & (1ULL << KPF_COMPOUND_TAIL)))
				break;
			native_pages++;
		}
		while (offset + slices * SLICE_SIZE < FILE_SIZE) {
			uint64_t next_pfn;

			if (virtual_pfn(pagemap_fd, map + offset +
					       slices * SLICE_SIZE, &next_pfn) ||
			    next_pfn != pfn)
				break;
			slices++;
		}
		if (native_pages < 2 ||
		    offset + native_pages * slices * SLICE_SIZE > FILE_SIZE)
			continue;

		info->file_offset = offset;
		info->native_pages = native_pages;
		info->slices_per_native = slices;
		rc = 0;
		break;
	}
out:
	if (flags_fd >= 0)
		close(flags_fd);
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	return rc;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t len = strlen(text);
	int rc = -1;

	if (fd >= 0 && write(fd, text, len) == len)
		rc = 0;
	if (fd >= 0)
		close(fd);
	return rc;
}

static int setup_reclaim_cgroup(char *path, size_t path_size)
{
	char procs[256];
	char pid[32];

	if (write_text("/sys/fs/cgroup/cgroup.subtree_control", "+memory\n") &&
	    errno != EBUSY)
		return -1;
	if (snprintf(path, path_size, "/sys/fs/cgroup/ttu-mlock-%d", getpid()) >=
	    (int)path_size || mkdir(path, 0755))
		return -1;
	if (snprintf(procs, sizeof(procs), "%s/cgroup.procs", path) >=
	    (int)sizeof(procs))
		return -1;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	return write_text(procs, pid);
}

static int reclaim_cgroup(const char *path)
{
	char reclaim[256];
	int fd;
	int rc;

	if (snprintf(reclaim, sizeof(reclaim), "%s/memory.reclaim", path) >=
	    (int)sizeof(reclaim))
		return -1;
	fd = open(reclaim, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = write(fd, "32M\n", 4);
	if (rc < 0 && errno == EAGAIN)
		rc = 0;
	close(fd);
	return rc < 0 ? -1 : 0;
}

static void cleanup_reclaim_cgroup(const char *path)
{
	char pid[32];

	if (!*path)
		return;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	write_text("/sys/fs/cgroup/cgroup.procs", pid);
	rmdir(path);
}

static int partial_large_folio_is_mlocked(unsigned char *map)
{
	int pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	int flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	uint64_t flags;
	uint64_t pfn;
	int ret = 0;

	if (pagemap_fd < 0 || flags_fd < 0)
		ret = -1;
	else if (virtual_pfn(pagemap_fd, map, &pfn))
		ret = 0;
	else if (read_u64(flags_fd, pfn, &flags))
		ret = -1;
	else
		ret = !!(flags & ((1ULL << KPF_COMPOUND_HEAD) |
				   (1ULL << KPF_COMPOUND_TAIL))) &&
		      !!(flags & (1ULL << KPF_MLOCKED)) &&
		      !!(flags & (1ULL << KPF_UNEVICTABLE));

	if (flags_fd >= 0)
		close(flags_fd);
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	return ret;
}

int main(int argc, char **argv)
{
	char path[PATH_MAX] = "ttu-partial-mlock-ppps.XXXXXX";
	char cgroup_path[256] = {};
	struct large_folio info = {};
	unsigned char *map = MAP_FAILED;
	size_t partial_slices;
	size_t map_len = FILE_SIZE;
	size_t offset;
	int fd = -1;
	int premature;
	int rc = 0;
	size_t checksum = 0;

	printf("TAP version 13\n1..6\n");
	if (argc > 2) {
		printf("Bail out! usage: %s [FILE]\n", argv[0]);
		return 1;
	}
	result(sysconf(_SC_PAGESIZE) == SLICE_SIZE,
	       "process page size is 4K");
	if (setup_reclaim_cgroup(cgroup_path, sizeof(cgroup_path))) {
		skip("locate a large file folio", "memcg reclaim is unavailable");
		skip("fault only part of a locked large folio",
		     "memcg reclaim is unavailable");
		skip("partial large folio starts reclaimable",
		     "memcg reclaim is unavailable");
		skip("trigger memcg reclaim", "memcg reclaim is unavailable");
		skip("reclaim does not mlock a partially mapped large folio",
		     "memcg reclaim is unavailable");
		cleanup_reclaim_cgroup(cgroup_path);
		printf("# Totals: pass:%d fail:%d\n",
		       test_no - failures, failures);
		return failures ? 1 : KSFT_SKIP;
	}

	if (argc == 2) {
		strncpy(path, argv[1], sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';
		fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	} else {
		fd = mkstemp(path);
	}
	if (fd < 0 || prepare_file(fd))
		rc = -1;
	if (!rc)
		map = mmap(MAP_ADDR, FILE_SIZE, PROT_READ,
			   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (map == MAP_FAILED && !rc)
		rc = -1;
	if (!rc) {
		for (offset = 0; offset < FILE_SIZE; offset += SLICE_SIZE)
			checksum += map[offset];
		rc = find_large_folio(map, &info);
	}
	result(!rc, "locate a large file folio");
	if (rc)
		goto skip_remaining;

	printf("# checksum=%zu file_offset=%zu native_pages=%zu slices_per_native=%zu\n",
	       checksum, info.file_offset,
	       info.native_pages, info.slices_per_native);
	munmap(map, FILE_SIZE);
	map = MAP_FAILED;

	partial_slices = info.slices_per_native > 1 ? info.native_pages :
			 (info.native_pages / 2 ? info.native_pages / 2 : 1);
	map_len = partial_slices * SLICE_SIZE;
	map = mmap(MAP_ADDR, map_len, PROT_READ,
		   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, info.file_offset);
	if (map == MAP_FAILED || mlock2(map, map_len, MLOCK_ONFAULT))
		rc = -1;
	if (!rc) {
		for (offset = 0; offset < partial_slices * SLICE_SIZE;
		     offset += SLICE_SIZE)
			checksum += map[offset];
		printf("# partial_checksum=%zu\n", checksum);
	}
	result(!rc, "fault only part of a locked large folio");
	if (rc)
		goto skip_pressure;

	premature = partial_large_folio_is_mlocked(map);
	if (premature < 0) {
		skip("partial large folio starts reclaimable",
		     "PFNs or kpageflags are unavailable");
		goto skip_pressure_result;
	}
	result(premature == 0, "partial large folio starts reclaimable");

	rc = write_text("/proc/self/clear_refs", "1\n");
	if (!rc)
		rc = reclaim_cgroup(cgroup_path);
	if (!rc)
		rc = reclaim_cgroup(cgroup_path);
	result(!rc, "trigger memcg reclaim");
	if (rc)
		goto skip_final;

	premature = partial_large_folio_is_mlocked(map);
	printf("# compound_mlocked_after_reclaim=%d\n", premature);
	if (premature < 0)
		skip("reclaim does not mlock a partially mapped large folio",
		     "PFNs or kpageflags are unavailable");
	else
		result(premature == 0,
		       "reclaim does not mlock a partially mapped large folio");
	goto out;

skip_remaining:
	skip("fault only part of a locked large folio", "setup failed");
	skip("partial large folio starts reclaimable", "setup failed");
	skip("trigger memcg reclaim", "setup failed");
	skip("reclaim does not mlock a partially mapped large folio", "setup failed");
	goto out;
skip_pressure:
	skip("partial large folio starts reclaimable", "setup failed");
skip_pressure_result:
	skip("trigger memcg reclaim", "setup failed");
	skip("reclaim does not mlock a partially mapped large folio", "setup failed");
	goto out;
skip_final:
	skip("reclaim does not mlock a partially mapped large folio", "pressure failed");
out:
	if (map != MAP_FAILED) {
		munlock(map, map_len);
		munmap(map, map_len);
	}
	if (fd >= 0)
		close(fd);
	unlink(path);
	cleanup_reclaim_cgroup(cgroup_path);
	printf("# Totals: pass:%d fail:%d\n", test_no - failures, failures);
	return failures ? 1 : 0;
}
