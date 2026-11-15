// SPDX-License-Identifier: GPL-2.0
/*
 * memcg reclaim of a large file folio that a 4K compat process has only
 * partially mapped under MLOCK_ONFAULT does not mlock the whole folio: the
 * partially mapped folio stays reclaimable before and after the pressure.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define FILE_SIZE (2UL * 1024 * 1024)
#define MAP_ADDR ((void *)0x40001000UL)
#define P3S_KPF_UNEVICTABLE BIT_ULL(18)
#define P3S_KPF_MLOCKED BIT_ULL(33)

struct large_folio {
	size_t file_offset;
	size_t native_pages;
	size_t slices_per_native;
};

static int read_u64(int fd, uint64_t index, uint64_t *value)
{
	return pread(fd, value, sizeof(*value), index * sizeof(*value)) ==
	       sizeof(*value) ? 0 : -1;
}

static int virtual_pfn(int pagemap_fd, const void *address, uint64_t *pfn)
{
	uint64_t entry;

	if (read_u64(pagemap_fd, (uintptr_t)address / getpagesize(), &entry) ||
	    !(entry & PM_PRESENT))
		return -1;
	*pfn = PAGEMAP_PFN(entry);
	return *pfn ? 0 : -1;
}

static int prepare_file(int fd)
{
	unsigned char buffer[PROCESS_PAGE_SIZE];
	size_t offset;
	int rc;

	for (offset = 0; offset < FILE_SIZE; offset += sizeof(buffer)) {
		memset(buffer, (offset / PROCESS_PAGE_SIZE) % 251 + 1, sizeof(buffer));
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

/* Returns 0 when found, 1 when the file has no large folio, -1 on error. */
static int find_large_folio(unsigned char *map, struct large_folio *info)
{
	int pagemap_fd = -1;
	int flags_fd = -1;
	size_t offset;
	int rc = 1;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0 || flags_fd < 0) {
		rc = 3;
		goto out;
	}

	for (offset = 0; offset < FILE_SIZE; offset += getpagesize()) {
		uint64_t flags, pfn;
		size_t frames = 1;
		size_t slices = NATIVE_PAGE_SIZE / getpagesize();

		if (virtual_pfn(pagemap_fd, map + offset, &pfn)) {
			rc = 3;
			goto out;
		}
		if (read_u64(flags_fd, pfn, &flags)) {
			rc = -1;
			goto out;
		}
		if (!(flags & KPF_COMPOUND_HEAD))
			continue;
		while (frames < 512 * slices) {
			if (read_u64(flags_fd, pfn + frames, &flags)) {
				rc = -1;
				goto out;
			}
			if (!(flags & KPF_COMPOUND_TAIL))
				break;
			frames++;
		}
		if (frames <= slices || frames % slices ||
		    offset + frames * getpagesize() > FILE_SIZE)
			continue;
		info->file_offset = offset;
		info->native_pages = frames / slices;
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

static int setup_reclaim_cgroup(char *path, size_t path_size,
				bool *restore_memory, bool *created)
{
	char procs[256];
	char pid[32];
	char controls[256];
	FILE *file;

	file = fopen("/sys/fs/cgroup/cgroup.subtree_control", "re");
	if (!file)
		return -1;
	if (!fgets(controls, sizeof(controls), file)) {
		fclose(file);
		return -1;
	}
	fclose(file);
	if (!strstr(controls, "memory")) {
		if (write_text("/sys/fs/cgroup/cgroup.subtree_control", "+memory\n"))
			return -1;
		*restore_memory = true;
	}
	if (snprintf(path, path_size, "/sys/fs/cgroup/ttu-mlock-%d", getpid()) >=
	    (int)path_size)
		return -1;
	if (mkdir(path, 0755))
		return -1;
	*created = true;
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
		ret = !!(flags & (KPF_COMPOUND_HEAD | KPF_COMPOUND_TAIL)) &&
		      !!(flags & P3S_KPF_MLOCKED) &&
		      !!(flags & P3S_KPF_UNEVICTABLE);

	if (flags_fd >= 0)
		close(flags_fd);
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	return ret;
}

static const char *test_file;

FIXTURE(partial_lock) {
	char path[PATH_MAX];
	char cgroup_path[256];
	bool created_file;
	bool created_cgroup;
	bool restore_memory;
	unsigned char *map;
	size_t map_len;
};

FIXTURE_SETUP(partial_lock)
{
}

/* Setup runs in the TEST body so parent teardown still runs after a SKIP. */
static int prepare_partial_lock(struct _test_data_partial_lock *self)
{
	struct large_folio info = {};
	size_t partial_slices;
	size_t checksum = 0;
	unsigned char *map;
	int fd, rc;

	if (setup_reclaim_cgroup(self->cgroup_path,
				 sizeof(self->cgroup_path),
				 &self->restore_memory, &self->created_cgroup))
		return 1;
	if (test_file) {
		if (snprintf(self->path, sizeof(self->path), "%s", test_file) >=
		    sizeof(self->path))
			return -1;
		fd = open(self->path,
			  O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	} else {
		strcpy(self->path, "ttu-partial-mlock-p3s.XXXXXX");
		fd = mkstemp(self->path);
	}
	if (fd < 0)
		return -1;
	self->created_file = true;
	if (prepare_file(fd))
		return -1;
	map = mmap(MAP_ADDR, FILE_SIZE, PROT_READ,
		   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (map != MAP_ADDR)
		return -1;
	for (size_t offset = 0; offset < FILE_SIZE; offset += PROCESS_PAGE_SIZE)
		checksum += map[offset];
	rc = find_large_folio(map, &info);
	if (rc == 1)
		return 2;
	if (rc == 3)
		return 3;
	if (rc || munmap(map, FILE_SIZE))
		return -1;
	ksft_print_msg("large folio checksum=%zu offset=%zu native_pages=%zu slices=%zu\n",
		       checksum, info.file_offset, info.native_pages,
		       info.slices_per_native);
	partial_slices = info.slices_per_native > 1 ? info.native_pages :
		(info.native_pages / 2 ? info.native_pages / 2 : 1);
	self->map_len = partial_slices * PROCESS_PAGE_SIZE;
	self->map = mmap(MAP_ADDR, self->map_len, PROT_READ,
			 MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd,
			 info.file_offset);
	if (self->map != MAP_ADDR ||
	    mlock2(self->map, self->map_len, MLOCK_ONFAULT))
		return -1;
	for (size_t offset = 0; offset < self->map_len;
	     offset += PROCESS_PAGE_SIZE)
		checksum += self->map[offset];
	ksft_print_msg("partial checksum=%zu\n", checksum);
	close(fd);
	return 0;
}

/* The child may abort after joining the cgroup or creating the file. */
FIXTURE_TEARDOWN_PARENT(partial_lock)
{
	if (self->created_cgroup)
		EXPECT_EQ(0, rmdir(self->cgroup_path));
	if (self->restore_memory)
		EXPECT_EQ(0, write_text("/sys/fs/cgroup/cgroup.subtree_control",
					"-memory\n"));
	if (self->created_file)
		EXPECT_EQ(0, unlink(self->path));
}

TEST_F(partial_lock, partial_large_folio_starts_reclaimable)
{
	int rc, mlocked;

	if (access("/sys/kernel/mm/transparent_hugepage", F_OK))
		SKIP(return, "kernel lacks transparent hugepage / large-folio support");
	rc = prepare_partial_lock(self);

	if (rc == 1)
		SKIP(return, "memcg reclaim is unavailable");
	if (rc == 2)
		SKIP(return, "filesystem did not allocate a large folio");
	if (rc == 3)
		SKIP(return, "pagemap PFNs or kpageflags are unavailable");
	ASSERT_EQ(0, rc);
	mlocked = partial_large_folio_is_mlocked(self->map);
	if (mlocked < 0)
		SKIP(return, "PFNs or kpageflags are unavailable");
	EXPECT_EQ(0, mlocked);
}

TEST_F(partial_lock, reclaim_does_not_mlock_partial_large_folio)
{
	int rc, mlocked;

	if (access("/sys/kernel/mm/transparent_hugepage", F_OK))
		SKIP(return, "kernel lacks transparent hugepage / large-folio support");
	rc = prepare_partial_lock(self);

	if (rc == 1)
		SKIP(return, "memcg reclaim is unavailable");
	if (rc == 2)
		SKIP(return, "filesystem did not allocate a large folio");
	if (rc == 3)
		SKIP(return, "pagemap PFNs or kpageflags are unavailable");
	ASSERT_EQ(0, rc);
	mlocked = partial_large_folio_is_mlocked(self->map);
	if (mlocked < 0)
		SKIP(return, "PFNs or kpageflags are unavailable");
	ASSERT_EQ(0, mlocked);
	ASSERT_EQ(0, write_text("/proc/self/clear_refs", "1\n"));
	ASSERT_EQ(0, reclaim_cgroup(self->cgroup_path));
	ASSERT_EQ(0, reclaim_cgroup(self->cgroup_path));
	mlocked = partial_large_folio_is_mlocked(self->map);
	TH_LOG("compound_mlocked_after_reclaim=%d", mlocked);
	if (mlocked < 0)
		SKIP(return, "PFNs or kpageflags are unavailable after reclaim");
	EXPECT_EQ(0, mlocked);
}

static void setup_harness(int *argc, char ***argv)
{
	test_file = p3s_harness_take_positional(argc, argv);
}

P3S_HARNESS_4KB_MAIN
