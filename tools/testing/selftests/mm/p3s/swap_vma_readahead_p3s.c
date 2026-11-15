// SPDX-License-Identifier: GPL-2.0
/*
 * Verify swap I/O and byte integrity on a temporary non-rotational loop
 * swap device. Native sequential faults prefetch an adjacent native page.
 * P3S restores sibling slices together and keeps the baseline process-PTE
 * history, so the same native-page stride does not request extra readahead.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/loop.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <unistd.h>

#include "p3s_slice_test.h"
#define P3S_HARNESS_NATIVE_CONTROL 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define SWAP_FILE_SIZE	(128UL * 1024 * 1024)
#define FILLER_SIZE	(32UL * 1024 * 1024)
#define SWAP_PRIORITY	32000
#define SWAP_VERSION_OFF	1024
#define SWAP_LAST_PAGE_OFF	1028
#define SWAP_MAGIC		"SWAPSPACE2"
#define SWAP_MAGIC_SIZE		10

#define ADDRESS_ALIGN	(64 * 1024UL)
#define TEST_PAGES	64
#define TEST_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(TEST_SIZE + 2 * ADDRESS_ALIGN)

static const char *page_cluster_path = "/proc/sys/vm/page-cluster";
static const char *vma_ra_path = "/sys/kernel/mm/swap/vma_ra_enabled";
static const char *cgroup_subtree_path = "/sys/fs/cgroup/cgroup.subtree_control";

static bool read_named_value(const char *path, const char *name,
			     unsigned long *value)
{
	char key[64];
	unsigned long current;
	char *line = NULL;
	size_t capacity = 0;
	FILE *file;

	file = fopen(path, "re");
	if (!file)
		return false;
	while (getline(&line, &capacity, file) >= 0) {
		if (sscanf(line, "%63s %lu", key, &current) == 2 &&
		    !strcmp(key, name)) {
			*value = current;
			free(line);
			fclose(file);
			return true;
		}
	}
	free(line);
	fclose(file);
	return false;
}

static bool read_ulong_file(const char *path, unsigned long *value)
{
	FILE *file = fopen(path, "re");
	bool success = file && fscanf(file, "%lu", value) == 1;

	if (file)
		fclose(file);
	return success;
}

static bool read_word_file(const char *path, char *word, size_t size)
{
	FILE *file = fopen(path, "re");
	bool success = file && fgets(word, size, file);

	if (file)
		fclose(file);
	if (success)
		word[strcspn(word, "\n")] = '\0';
	return success && word[0];
}

static bool check_existing_swaps(bool *has_other_swap)
{
	char line[512], device[256], type[64], path[320];
	unsigned long rotational;
	const char *base;
	FILE *swaps;
	bool ok = true;

	*has_other_swap = false;
	swaps = fopen("/proc/swaps", "re");
	if (!swaps)
		return true;
	if (!fgets(line, sizeof(line), swaps))
		goto out;
	while (fgets(line, sizeof(line), swaps)) {
		if (sscanf(line, "%255s %63s", device, type) != 2)
			continue;
		*has_other_swap = true;
		if (strcmp(type, "partition")) {
			ok = false;
			break;
		}
		base = strrchr(device, '/');
		base = base ? base + 1 : device;
		if (snprintf(path, sizeof(path),
			     "/sys/class/block/%s/queue/rotational", base) <
		    (int)sizeof(path) &&
		    read_ulong_file(path, &rotational) && rotational) {
			ok = false;
			break;
		}
	}
out:
	fclose(swaps);
	return ok;
}

static long swap_used_kb(const char *dev)
{
	char line[512], name[PATH_MAX];
	long size, used;
	FILE *file = fopen("/proc/swaps", "re");

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%4095s %*s %ld %ld", name, &size, &used) == 3 &&
		    !strcmp(name, dev)) {
			fclose(file);
			return used;
		}
	}
	fclose(file);
	return -1;
}

static int open_temp_swapfile(char *path, size_t size)
{
	const char *dir = getenv("TMPDIR");
	int fd = -1;

	if (dir && *dir &&
	    snprintf(path, size, "%s/p3s-ra-swap-XXXXXX", dir) < (int)size)
		fd = mkstemp(path);
	if (fd < 0 &&
	    snprintf(path, size, "./p3s-ra-swap-XXXXXX") < (int)size)
		fd = mkstemp(path);
	return fd;
}

static bool pin_one_cpu(void)
{
	cpu_set_t allowed, one;
	int cpu;

	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		return false;
	CPU_ZERO(&one);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &allowed)) {
			CPU_SET(cpu, &one);
			return !sched_setaffinity(0, sizeof(one), &one);
		}
	}
	return false;
}

struct ra_cgroup {
	char parent[PATH_MAX];
	char child[PATH_MAX];
	char reclaim[PATH_MAX + 32];
	bool active;
};

static bool enter_ra_cgroup(struct ra_cgroup *cg)
{
	char controllers[128], cgroup[PATH_MAX], pid[32];
	char procs[PATH_MAX + 32];
	char *line = NULL;
	size_t cap = 0;
	FILE *file;
	bool found = false;

	memset(cg, 0, sizeof(*cg));
	file = fopen(cgroup_subtree_path, "re");
	if (!file)
		return false;
	controllers[0] = '\0';
	if (fgets(controllers, sizeof(controllers), file))
		found = strstr(controllers, "memory") != NULL;
	fclose(file);
	if (!found)
		return false;

	found = false;
	file = fopen("/proc/self/cgroup", "re");
	if (!file)
		return false;
	while (getline(&line, &cap, file) >= 0) {
		if (sscanf(line, "0::%4095[^\n]", cgroup) == 1) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(file);
	if (!found ||
	    snprintf(cg->parent, sizeof(cg->parent), "/sys/fs/cgroup%s/cgroup.procs",
		     cgroup) >= (int)sizeof(cg->parent) ||
	    snprintf(cg->child, sizeof(cg->child), "/sys/fs/cgroup/p3s-ra-%ld",
		     (long)getpid()) >= (int)sizeof(cg->child) ||
	    snprintf(cg->reclaim, sizeof(cg->reclaim), "%s/memory.reclaim",
		     cg->child) >= (int)sizeof(cg->reclaim))
		return false;
	if (mkdir(cg->child, 0755))
		return false;
	snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
	snprintf(procs, sizeof(procs), "%s/cgroup.procs", cg->child);
	if (p3s_write_text(procs, pid)) {
		rmdir(cg->child);
		return false;
	}
	cg->active = true;
	return true;
}

static void leave_ra_cgroup(struct ra_cgroup *cg)
{
	char pid[32];

	if (!cg->active)
		return;
	snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
	p3s_write_text(cg->parent, pid);
	rmdir(cg->child);
	cg->active = false;
}

static bool reclaim_cgroup_bytes(struct ra_cgroup *cg, unsigned long bytes)
{
	char req[64];

	snprintf(req, sizeof(req), "%lu swappiness=max\n", bytes);
	return !p3s_write_text(cg->reclaim, req) || errno == EAGAIN;
}

static bool swap_via_cgroup(struct ra_cgroup *cg, void *addr,
			    unsigned long len, unsigned long minimum)
{
	unsigned long swap = 0;
	unsigned int i;

	if (madvise(addr, len, MADV_PAGEOUT))
		return false;
	for (i = 0; i < 200; i++) {
		if (p3s_smaps_bytes(addr, len, "Swap", &swap) && swap >= minimum)
			return true;
		reclaim_cgroup_bytes(cg, len);
		usleep(10000);
	}
	return false;
}

/* mincore reports up-to-date swap-cache folios for swapped anonymous PTEs. */
static bool cache_absent(unsigned char *mapping, unsigned int page)
{
	unsigned char resident[P3S_SLICES];
	unsigned int i;

	if (mincore(mapping + page * NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		    resident))
		return false;
	for (i = 0; i < NATIVE_PAGE_SIZE / (unsigned int)getpagesize(); i++) {
		if (resident[i] & 1)
			return false;
	}
	return true;
}

static bool cache_present(unsigned char *mapping, unsigned int page)
{
	unsigned char resident[P3S_SLICES];
	unsigned int i;

	if (mincore(mapping + page * NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		    resident))
		return false;
	for (i = 0; i < NATIVE_PAGE_SIZE / (unsigned int)getpagesize(); i++) {
		if (!(resident[i] & 1))
			return false;
	}
	return true;
}

static bool test_pages_cold(unsigned char *mapping)
{
	return cache_absent(mapping, 0) && cache_absent(mapping, 8) &&
	       cache_absent(mapping, 9) && cache_absent(mapping, 10);
}

static bool evict_swap_cache_cgroup(struct ra_cgroup *cg,
				    unsigned char *mapping)
{
	unsigned int i;

	for (i = 0; i < 30; i++) {
		if (test_pages_cold(mapping))
			return true;
		reclaim_cgroup_bytes(cg, 4UL * 1024 * 1024);
		usleep(10000);
	}
	return test_pages_cold(mapping);
}

static long major_faults(void)
{
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage))
		return -1;
	return usage.ru_majflt;
}

static bool fault_and_check(unsigned char *mapping, unsigned int page,
			    long *major_delta)
{
	long before = major_faults();
	unsigned char value = ((volatile unsigned char *)mapping)[page * NATIVE_PAGE_SIZE];
	long after = major_faults();

	if (before < 0 || after < 0)
		return false;
	*major_delta = after - before;
	return value == (unsigned char)(0x40 + page);
}

static int run_readahead_case(bool expect_4kb, const char *swap_dev,
			      bool has_other_swap)
{
	unsigned long swap_ra_before = 0, swap_ra_after = 0;
	unsigned long rss_bytes = 0, swap_bytes = 0;
	unsigned char *reservation = MAP_FAILED, *mapping = MAP_FAILED;
	unsigned char *filler = MAP_FAILED;
	struct ra_cgroup cg = {};
	uintptr_t aligned;
	long major_delta[3] = {};
	long used_before, used_after;
	unsigned int i;
	int ret = KSFT_FAIL;

	if (!pin_one_cpu() || !enter_ra_cgroup(&cg))
		return KSFT_SKIP;

	if (has_other_swap) {
		filler = mmap(NULL, FILLER_SIZE, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (filler == MAP_FAILED)
			goto out;
		for (i = 0; i < FILLER_SIZE / NATIVE_PAGE_SIZE; i++)
			filler[i * NATIVE_PAGE_SIZE] = (unsigned char)i;
		if (!swap_via_cgroup(&cg, filler, FILLER_SIZE,
				     FILLER_SIZE / 2)) {
			ret = KSFT_SKIP;
			goto out;
		}
	}

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto out;
	aligned = ((uintptr_t)reservation + 2 * ADDRESS_ALIGN - 1) &
		  ~(uintptr_t)(ADDRESS_ALIGN - 1);
	mapping = mmap((void *)aligned, TEST_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping != (unsigned char *)aligned ||
	    (aligned & (ADDRESS_ALIGN - 1)) ||
	    (madvise(mapping, TEST_SIZE, MADV_NOHUGEPAGE) && errno != EINVAL) ||
	    p3s_maps_count(mapping, TEST_SIZE) != 1)
		goto out;

	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i / P3S_SLICES;

	used_before = swap_used_kb(swap_dev);
	if (!swap_via_cgroup(&cg, mapping, TEST_SIZE, TEST_SIZE / 2) ||
	    !p3s_smaps_bytes(mapping, TEST_SIZE, "Rss", &rss_bytes) ||
	    !p3s_smaps_bytes(mapping, TEST_SIZE, "Swap", &swap_bytes)) {
		ret = KSFT_SKIP;
		goto out;
	}
	used_after = swap_used_kb(swap_dev);
	ksft_print_msg("%s: Swap=%lu Rss=%lu loop_used=%ld->%ld\n",
		       expect_4kb ? "4KB" : "16KB", swap_bytes, rss_bytes,
		       used_before, used_after);
	if (used_before >= 0 && used_after <= used_before) {
		ret = KSFT_SKIP;
		goto out;
	}
	if (!evict_swap_cache_cgroup(&cg, mapping)) {
		ret = KSFT_SKIP;
		goto out;
	}

	if (!fault_and_check(mapping, 0, &major_delta[0]) ||
	    major_delta[0] <= 0 ||
	    !fault_and_check(mapping, 8, &major_delta[1]) ||
	    major_delta[1] <= 0)
		goto out;
	if (!cache_absent(mapping, 9) || !cache_absent(mapping, 10)) {
		ret = KSFT_SKIP;
		goto out;
	}
	if (!read_named_value("/proc/vmstat", "swap_ra", &swap_ra_before) ||
	    !fault_and_check(mapping, 9, &major_delta[2]) ||
	    major_delta[2] <= 0 ||
	    !read_named_value("/proc/vmstat", "swap_ra", &swap_ra_after))
		goto out;

	ksft_print_msg("%s: major=%ld,%ld,%ld swap_ra=%lu->%lu\n",
		       expect_4kb ? "4KB" : "16KB",
		       major_delta[0], major_delta[1], major_delta[2],
		       swap_ra_before, swap_ra_after);
	if (expect_4kb) {
		if (cache_absent(mapping, 10) &&
		    swap_ra_after == swap_ra_before)
			ret = KSFT_PASS;
	} else {
		if (cache_present(mapping, 10) &&
		    swap_ra_after > swap_ra_before)
			ret = KSFT_PASS;
	}
out:
	if (reservation != MAP_FAILED)
		munmap(reservation, RESERVE_SIZE);
	if (filler != MAP_FAILED)
		munmap(filler, FILLER_SIZE);
	leave_ra_cgroup(&cg);
	return ret;
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--native-check")) {
		if (getpagesize() != NATIVE_PAGE_SIZE)
			return KSFT_SKIP;
		return run_readahead_case(false, argv[2], atoi(argv[3]) != 0);
	}
	return -1;
}

FIXTURE(swap_readahead) {
	unsigned long saved_page_cluster;
	char saved_vma_ra[16];
	char swap_dev[64];
	bool swap_active;
	bool has_other_swap;
	bool restore_page_cluster;
	bool restore_vma_ra;
};

FIXTURE_SETUP(swap_readahead)
{
	struct loop_info64 info = { .lo_flags = LO_FLAGS_AUTOCLEAR };
	char path[PATH_MAX], rot_path[128];
	unsigned char *header;
	unsigned long rotational = 0;
	const char *base;
	int file_fd = -1, loop_fd = -1, ctl = -1, index, i, err;
	bool ok_pc, ok_ra;

	if (geteuid())
		SKIP(return, "root is required to set up loop swap");
	if (!check_existing_swaps(&self->has_other_swap))
		SKIP(return, "rotational or unsupported swap device is active");

	file_fd = open_temp_swapfile(path, sizeof(path));
	if (file_fd < 0)
		SKIP(return, "cannot create temporary file: %s",
		     strerror(errno));
	unlink(path);
	if (ftruncate(file_fd, SWAP_FILE_SIZE)) {
		err = errno;
		close(file_fd);
		ASSERT_EQ(0, err);
	}

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		ctl = open("/dev/block/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		err = errno;
		close(file_fd);
		SKIP(return, "loop-control unavailable: %s", strerror(err));
	}
	index = ioctl(ctl, LOOP_CTL_GET_FREE);
	err = errno;
	close(ctl);
	if (index < 0) {
		close(file_fd);
		SKIP(return, "no free loop device: %s", strerror(err));
	}

	snprintf(self->swap_dev, sizeof(self->swap_dev),
		 "/dev/block/loop%d", index);
	loop_fd = open(self->swap_dev, O_RDWR | O_CLOEXEC);
	for (i = 0; loop_fd < 0 && errno == ENOENT && i < 20; i++) {
		usleep(50000);
		loop_fd = open(self->swap_dev, O_RDWR | O_CLOEXEC);
	}
	if (loop_fd < 0) {
		snprintf(self->swap_dev, sizeof(self->swap_dev),
			 "/dev/loop%d", index);
		loop_fd = open(self->swap_dev, O_RDWR | O_CLOEXEC);
	}
	if (loop_fd < 0 || ioctl(loop_fd, LOOP_SET_FD, file_fd)) {
		err = errno;
		if (loop_fd >= 0)
			close(loop_fd);
		close(file_fd);
		SKIP(return, "cannot attach loop device: %s", strerror(err));
	}
	close(file_fd);
	if (ioctl(loop_fd, LOOP_SET_STATUS64, &info)) {
		err = errno;
		ioctl(loop_fd, LOOP_CLR_FD, 0);
		close(loop_fd);
		ASSERT_EQ(0, err);
	}

	base = strrchr(self->swap_dev, '/');
	base = base ? base + 1 : self->swap_dev;
	snprintf(rot_path, sizeof(rot_path),
		 "/sys/class/block/%s/queue/rotational", base);
	if (!read_ulong_file(rot_path, &rotational) || rotational) {
		close(loop_fd);
		SKIP(return, "loop device is marked rotational");
	}

	header = calloc(1, NATIVE_PAGE_SIZE);
	if (!header) {
		close(loop_fd);
		ASSERT_NE(NULL, header);
	}
	*(uint32_t *)(header + SWAP_VERSION_OFF) = 1;
	*(uint32_t *)(header + SWAP_LAST_PAGE_OFF) =
		SWAP_FILE_SIZE / NATIVE_PAGE_SIZE - 1;
	memcpy(header + NATIVE_PAGE_SIZE - SWAP_MAGIC_SIZE,
	       SWAP_MAGIC, SWAP_MAGIC_SIZE);
	if (pwrite(loop_fd, header, NATIVE_PAGE_SIZE, 0) != NATIVE_PAGE_SIZE ||
	    fsync(loop_fd)) {
		err = errno;
		free(header);
		close(loop_fd);
		ASSERT_EQ(0, err);
	}
	free(header);

	ok_pc = read_ulong_file(page_cluster_path, &self->saved_page_cluster);
	ok_ra = read_word_file(vma_ra_path, self->saved_vma_ra,
			       sizeof(self->saved_vma_ra));
	if (!ok_pc || !ok_ra)
		close(loop_fd);
	ASSERT_TRUE(ok_pc);
	ASSERT_TRUE(ok_ra);

	if (swapon(self->swap_dev, SWAP_FLAG_PREFER |
				   (SWAP_PRIORITY << SWAP_FLAG_PRIO_SHIFT))) {
		err = errno;
		close(loop_fd);
		SKIP(return, "swapon %s failed: %s",
		     self->swap_dev, strerror(err));
	}
	close(loop_fd);
	self->swap_active = true;

	err = p3s_write_text(page_cluster_path, "1\n");
	if (err) {
		swapoff(self->swap_dev);
		self->swap_active = false;
		ASSERT_EQ(0, err);
	}
	self->restore_page_cluster = true;
	err = p3s_write_text(vma_ra_path, "true\n");
	if (err) {
		char value[32];

		snprintf(value, sizeof(value), "%lu\n",
			 self->saved_page_cluster);
		p3s_write_text(page_cluster_path, value);
		self->restore_page_cluster = false;
		swapoff(self->swap_dev);
		self->swap_active = false;
		ASSERT_EQ(0, err);
	}
	self->restore_vma_ra = true;
}

/* Always restore sysctls and detach the temporary loop swap device. */
FIXTURE_TEARDOWN_PARENT(swap_readahead)
{
	char value[32];

	if (self->restore_page_cluster) {
		snprintf(value, sizeof(value), "%lu\n",
			 self->saved_page_cluster);
		EXPECT_EQ(0, p3s_write_text(page_cluster_path, value));
	}
	if (self->restore_vma_ra)
		EXPECT_EQ(0, p3s_write_text(vma_ra_path, self->saved_vma_ra));
	if (self->swap_active)
		EXPECT_EQ(0, swapoff(self->swap_dev));
}

TEST_F(swap_readahead, adjacent_native_fault_uses_process_readahead_policy)
{
	bool is_4kb = getpagesize() == PROCESS_PAGE_SIZE;
	int ret;

	ret = run_readahead_case(is_4kb, self->swap_dev, self->has_other_swap);
	if (ret == KSFT_SKIP)
		SKIP(return, "swap readahead precondition unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
	if (is_4kb) {
		ret = p3s_run_child(false, "--native-check", self->swap_dev,
				    self->has_other_swap ? "1" : "0", NULL);
		if (ret == KSFT_SKIP)
			SKIP(return, "native readahead control unavailable");
		EXPECT_EQ(KSFT_PASS, ret);
	}
}

P3S_HARNESS_4KB_MAIN
