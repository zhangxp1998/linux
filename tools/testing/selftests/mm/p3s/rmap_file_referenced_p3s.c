// SPDX-License-Identifier: GPL-2.0
/*
 * In an isolated memory cgroup with MGLRU disabled, memory.reclaim keeps a
 * file folio resident when only its final 4K slice was referenced by a
 * compat process.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/magic.h>
#include <sys/vfs.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define NR_SLICES P3S_SLICES
#define MAP_SIZE (PROCESS_PAGE_SIZE * NR_SLICES)
#define FILLER_SIZE (2 * 1024 * 1024UL)

static int read_text(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	if (n < 0) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	buf[n] = '\0';
	close(fd);
	return 0;
}

FIXTURE(rmap_file) {
	char test_file[64];
	char lru_gen_value[32];
	char cgroup_path[PATH_MAX];
	bool restore_lru_gen;
	bool created_cgroup;
};

FIXTURE_SETUP(rmap_file)
{
}

/* Restore global MGLRU state even if the test child fails or crashes. */
FIXTURE_TEARDOWN_PARENT(rmap_file)
{
	if (self->created_cgroup && rmdir(self->cgroup_path))
		perror("remove test memory cgroup");
	if (self->restore_lru_gen &&
	    p3s_write_text("/sys/kernel/mm/lru_gen/enabled", self->lru_gen_value))
		perror("restore MGLRU");
	if (self->test_file[0])
		unlink(self->test_file);
}

TEST_F(rmap_file, last_4k_slice_keeps_file_folio_referenced)
{
	static const char lru_gen_path[] = "/sys/kernel/mm/lru_gen/enabled";
	static const char subtree[] = "/sys/fs/cgroup/cgroup.subtree_control";
	char procs[PATH_MAX + 32], reclaim[PATH_MAX + 32], controllers[256];
	unsigned char vec[NR_SLICES] = {};
	unsigned char checksum = 0;
	unsigned long referenced = 0;
	unsigned char *filler = MAP_FAILED;
	char pid[32];
	unsigned char *map = MAP_FAILED;
	struct statfs fs;
	int fd;
	size_t i;
	int rc;
	int filler_rc;
	int reclaim_errno;

	if (geteuid())
		SKIP(return, "root required for memory.reclaim");
	if (read_text(subtree, controllers, sizeof(controllers)) ||
	    !strstr(controllers, "memory"))
		SKIP(return, "memory cgroup controller is not enabled");
	if (read_text(lru_gen_path, self->lru_gen_value,
		      sizeof(self->lru_gen_value)))
		SKIP(return, "MGLRU control is unavailable");
	strcpy(self->test_file, "rmap-file-p3s.XXXXXX");
	fd = mkostemp(self->test_file, O_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, fstatfs(fd, &fs));
	if (fs.f_type == TMPFS_MAGIC)
		SKIP(return, "a disk-backed file is required, not tmpfs");
	self->restore_lru_gen = true;
	ASSERT_EQ(0, p3s_write_text(lru_gen_path, "0"));

	snprintf(self->cgroup_path, sizeof(self->cgroup_path),
		 "/sys/fs/cgroup/rmap-file-referenced-p3s-%d", getpid());
	ASSERT_EQ(0, mkdir(self->cgroup_path, 0755));
	self->created_cgroup = true;
	snprintf(procs, sizeof(procs), "%s/cgroup.procs", self->cgroup_path);
	snprintf(reclaim, sizeof(reclaim), "%s/memory.reclaim",
		 self->cgroup_path);
	snprintf(pid, sizeof(pid), "%ld", (long)getpid());
	ASSERT_EQ(0, p3s_write_text(procs, pid));

	ASSERT_EQ(0, ftruncate(fd, MAP_SIZE));
	for (i = 0; i < MAP_SIZE; i += PROCESS_PAGE_SIZE)
		ASSERT_EQ(1, pwrite(fd, "x", 1, i));
	ASSERT_EQ(0, fsync(fd));

	map = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	ASSERT_NE(map, MAP_FAILED);
	for (i = 0; i < NR_SLICES; i++)
		checksum ^= map[i * PROCESS_PAGE_SIZE];
	printf("# target checksum=%u\n", checksum);

	filler = mmap(NULL, FILLER_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (filler != MAP_FAILED) {
		memset(filler, 0x5a, FILLER_SIZE);
		filler_rc = madvise(filler, FILLER_SIZE, MADV_FREE);
	} else {
		filler_rc = -1;
	}
	rc = mincore(map, MAP_SIZE, vec);
	ASSERT_NE(filler, MAP_FAILED);
	ASSERT_EQ(0, filler_rc);
	ASSERT_EQ(0, rc);
	for (i = 0; i < NR_SLICES; i++)
		ASSERT_TRUE(vec[i] & 1);

	rc = madvise(map, MAP_SIZE, MADV_COLD);
	ASSERT_EQ(0, rc);

	checksum ^= map[3 * PROCESS_PAGE_SIZE];
	printf("# hot checksum=%u\n", checksum);
	memset(vec, 0, sizeof(vec));
	rc = mincore(map, MAP_SIZE, vec);
	ASSERT_EQ(0, rc);
	ASSERT_TRUE(vec[3] & 1);
	rc = p3s_smaps_bytes(map, MAP_SIZE, "Referenced", &referenced);
	printf("# Referenced=%lu bytes\n", referenced);
	ASSERT_TRUE(rc);
	ASSERT_GE(referenced, PROCESS_PAGE_SIZE);
	ASSERT_LE(referenced, NATIVE_PAGE_SIZE);

	errno = 0;
	/* Reclaim one native folio; a larger request can revisit the hot target. */
	rc = p3s_write_text(reclaim, "16384");
	reclaim_errno = rc ? errno : 0;
	if (rc && reclaim_errno != EAGAIN)
		perror("write memory.reclaim");
	printf("# memory.reclaim rc=%d errno=%d\n", rc, reclaim_errno);
	memset(vec, 0, sizeof(vec));
	rc = mincore(map, MAP_SIZE, vec);
	printf("# mincore after reclaim: %u %u %u %u\n",
	       vec[0] & 1, vec[1] & 1, vec[2] & 1, vec[3] & 1);
	EXPECT_TRUE(reclaim_errno == 0 || reclaim_errno == EAGAIN);
	EXPECT_EQ(0, rc);
	EXPECT_TRUE(vec[3] & 1);
	munmap(map, MAP_SIZE);
	munmap(filler, FILLER_SIZE);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
