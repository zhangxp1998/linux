// SPDX-License-Identifier: GPL-2.0
/*
 * Four 4K anonymous slices keep their data across pageout/swapin in any
 * order, partial MADV_DONTNEED, fork COW, concurrent swapin, reclaim churn,
 * compaction and swap pressure.  A PAGEOUT request or privilege check that
 * cannot establish the test precondition skips rather than failing.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define CONCURRENT_TUPLES 64
#define CHURN_TUPLES 256
#define COMPACT_TUPLES 1024
#define PRESSURE_TUPLES 512
#define PRESSURE_CHURN 512
#define MIN_PRESSURE_SWAP (16UL * 1024 * 1024)

struct mapping {
	unsigned char *base;
	unsigned char *reservation;
	size_t reservation_size;
};

struct worker {
	unsigned char *base;
	unsigned int slice;
};

struct test_cgroup {
	char parent[PATH_MAX];
	char child[PATH_MAX];
	bool active;
};

static struct mapping map_tuples(size_t size)
{
	struct mapping result = {
		.reservation_size = size + NATIVE_PAGE_SIZE,
	};

	result.base = map_aligned(size, &result.reservation);
	return result;
}

static void unmap_mapping(struct mapping *mapping)
{
	if (mapping->base != MAP_FAILED)
		munmap(mapping->reservation, mapping->reservation_size);
	mapping->base = MAP_FAILED;
}

static bool reclaim_self_cgroup(unsigned long bytes)
{
	char cgroup[PATH_MAX];
	char path[PATH_MAX + 32];
	char request[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *cgroups;
	int path_length;

	cgroups = fopen("/proc/self/cgroup", "re");
	if (!cgroups)
		return false;
	while (getline(&line, &capacity, cgroups) >= 0) {
		if (sscanf(line, "0::%4095[^\n]", cgroup) == 1) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(cgroups);
	if (!found)
		return false;
	path_length = snprintf(path, sizeof(path),
			       "/sys/fs/cgroup%s/memory.reclaim", cgroup);
	if (path_length < 0 || (size_t)path_length >= sizeof(path))
		return false;
	snprintf(request, sizeof(request), "%lu swappiness=max", bytes);
	return !p3s_write_text(path, request) || errno == EAGAIN;
}

static bool write_pid_to_cgroup(const char *directory)
{
	char path[PATH_MAX];
	char pid[32];
	int path_length;

	path_length = snprintf(path, sizeof(path), "%s/cgroup.procs", directory);
	if (path_length < 0 || (size_t)path_length >= sizeof(path))
		return false;
	snprintf(pid, sizeof(pid), "%ld", (long)getpid());
	return !p3s_write_text(path, pid);
}

static bool enter_private_cgroup(struct test_cgroup *group)
{
	char cgroup[PATH_MAX];
	char *line = NULL;
	size_t capacity = 0;
	FILE *cgroups;
	bool found = false;
	int length;

	memset(group, 0, sizeof(*group));
	cgroups = fopen("/proc/self/cgroup", "re");
	if (!cgroups)
		return false;
	while (getline(&line, &capacity, cgroups) >= 0) {
		if (sscanf(line, "0::%4095[^\n]", cgroup) == 1) {
			found = true;
			break;
		}
	}
	free(line);
	fclose(cgroups);
	if (!found)
		return false;
	length = snprintf(group->parent, sizeof(group->parent),
			  "/sys/fs/cgroup%s", cgroup);
	if (length < 0 || (size_t)length >= sizeof(group->parent))
		return false;
	/* The Android per-process cgroup is a leaf without delegated memory. */
	length = snprintf(group->child, sizeof(group->child),
			  "/sys/fs/cgroup/p3s-%ld", (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(group->child) ||
	    mkdir(group->child, 0700))
		return false;
	if (!write_pid_to_cgroup(group->child)) {
		rmdir(group->child);
		return false;
	}
	group->active = true;
	return true;
}

static void leave_private_cgroup(struct test_cgroup *group)
{
	if (!group->active)
		return;
	write_pid_to_cgroup(group->parent);
	rmdir(group->child);
	group->active = false;
}

static bool cgroup_reclaim_reaches_swap(void *address, unsigned long length,
					unsigned long minimum)
{
	unsigned long swap;
	unsigned int attempt;

	for (attempt = 0; attempt < PAGEOUT_POLLS; attempt++) {
		if (!p3s_smaps_bytes(address, length, "Swap", &swap))
			return false;
		if (swap >= minimum)
			return true;
		if (!reclaim_self_cgroup(length))
			return false;
		usleep(10000);
	}
	return false;
}

static unsigned long swap_free_bytes(void)
{
	unsigned long free_kb = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		return 0;
	while (getline(&line, &capacity, meminfo) >= 0)
		if (sscanf(line, "SwapFree: %lu kB", &free_kb) == 1)
			break;
	free(line);
	fclose(meminfo);
	return free_kb * 1024;
}

static bool tuple_has_one_folio(const unsigned char *base)
{
	uint64_t pfn[P3S_SLICES];

	return read_pfns(base, pfn) && same_pfn(pfn);
}

static int first_swapin_is_write(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x31);
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	memset(mapping.base + 2 * PROCESS_PAGE_SIZE, 0x73, PROCESS_PAGE_SIZE);
	passed = mapping.base[0] == 0x31 &&
		mapping.base[PROCESS_PAGE_SIZE] == 0x32 &&
		mapping.base[2 * PROCESS_PAGE_SIZE] == 0x73 &&
		mapping.base[3 * PROCESS_PAGE_SIZE] == 0x34;
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

static int read_swapin_write_pageout(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	unsigned char *slice;
	unsigned int attempt;
	bool swapped = false;
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x41);
	slice = mapping.base + PROCESS_PAGE_SIZE;
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	/*
	 * A read fault swaps the tuple in clean and writable.  The store that
	 * follows must dirty the PTE again, otherwise the next pageout drops
	 * the folio and the swap slot still holds the old contents.
	 */
	passed = slice[0] == 0x42;
	if (passed)
		memset(slice, 0x74, PROCESS_PAGE_SIZE);
	if (passed && !madvise(mapping.base, NATIVE_PAGE_SIZE, MADV_PAGEOUT)) {
		for (attempt = 0; attempt < PAGEOUT_POLLS; attempt++) {
			if (p3s_page_swapped(slice)) {
				swapped = true;
				break;
			}
			usleep(10000);
		}
	} else {
		passed = false;
	}
	passed = passed && mapping.base[0] == 0x41 &&
		slice[0] == 0x74 && slice[PROCESS_PAGE_SIZE - 1] == 0x74 &&
		mapping.base[2 * PROCESS_PAGE_SIZE] == 0x43 &&
		mapping.base[3 * PROCESS_PAGE_SIZE] == 0x44;
	unmap_mapping(&mapping);
	if (!passed)
		return KSFT_FAIL;
	return swapped ? KSFT_PASS : KSFT_SKIP;
}

static int private_file_cow_swapin(void)
{
	unsigned char *expected = NULL;
	unsigned char *mapping = MAP_FAILED;
	unsigned char *file_data = NULL;
	unsigned int slice;
	bool passed = false;
	bool swapped = false;
	int fd = -1;

	fd = memfd_create("p3s-private-cow", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE))
		goto out;
	expected = malloc(NATIVE_PAGE_SIZE);
	file_data = malloc(NATIVE_PAGE_SIZE);
	if (!expected || !file_data)
		goto out;
	for (slice = 0; slice < P3S_SLICES; slice++)
		memset(file_data + slice * PROCESS_PAGE_SIZE, 0x40 + slice,
		       PROCESS_PAGE_SIZE);
	if (pwrite(fd, file_data, NATIVE_PAGE_SIZE, 0) != NATIVE_PAGE_SIZE)
		goto out;
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED)
		goto out;
	for (slice = 0; slice < P3S_SLICES; slice++) {
		memset(mapping + slice * PROCESS_PAGE_SIZE, 0x70 + slice,
		       PROCESS_PAGE_SIZE);
		memset(expected + slice * PROCESS_PAGE_SIZE, 0x70 + slice,
		       PROCESS_PAGE_SIZE);
	}
	/* Do not report swapin coverage unless at least one COW slice swapped. */
	swapped = pageout_reaches_swap(mapping, NATIVE_PAGE_SIZE,
				       PROCESS_PAGE_SIZE);
	passed = !memcmp(mapping, expected, NATIVE_PAGE_SIZE) &&
		 pread(fd, file_data, NATIVE_PAGE_SIZE, 0) == NATIVE_PAGE_SIZE;
	for (slice = 0; passed && slice < P3S_SLICES; slice++)
		passed = file_data[slice * PROCESS_PAGE_SIZE] == 0x40 + slice;
out:
	if (mapping != MAP_FAILED)
		munmap(mapping, NATIVE_PAGE_SIZE);
	free(file_data);
	free(expected);
	if (fd >= 0)
		close(fd);
	if (!passed)
		return KSFT_FAIL;
	return swapped ? KSFT_PASS : KSFT_SKIP;
}

static int partial_tuple_pageout(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	bool swapped = false;
	bool passed;
	int stage = 0;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x41);
	passed = !madvise(mapping.base + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			  MADV_DONTNEED);
	if (passed)
		stage = 1;
	if (passed)
		swapped = pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
					       3 * PROCESS_PAGE_SIZE);
	if (swapped)
		stage = 2;
	passed = passed && mapping.base[3 * PROCESS_PAGE_SIZE] == 0x44 &&
		 mapping.base[0] == 0x41 &&
		 mapping.base[2 * PROCESS_PAGE_SIZE] == 0x43;
	if (passed)
		stage = 3;
	passed = passed && mapping.base[PROCESS_PAGE_SIZE] == 0;
	if (passed)
		stage = 4;
	if (passed)
		memset(mapping.base + PROCESS_PAGE_SIZE, 0x42, PROCESS_PAGE_SIZE);
	passed = passed && check_slices(mapping.base, 0x41);
	if (passed)
		stage = 5;
	if (!passed)
		ksft_print_msg("partial tuple failed after stage %d\n", stage);
	unmap_mapping(&mapping);
	if (!passed)
		return KSFT_FAIL;
	return swapped ? KSFT_PASS : KSFT_SKIP;
}

static int fork_swapped_tuple(bool fault_before_fork)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	pid_t child;
	int status;
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x51);
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	if (fault_before_fork && mapping.base[0] != 0x51) {
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	child = fork();
	if (!child) {
		memset(mapping.base + 3 * PROCESS_PAGE_SIZE, 0x7f, PROCESS_PAGE_SIZE);
		passed = mapping.base[0] == 0x51 &&
		 mapping.base[PROCESS_PAGE_SIZE] == 0x52 &&
		 mapping.base[2 * PROCESS_PAGE_SIZE] == 0x53 &&
		 mapping.base[3 * PROCESS_PAGE_SIZE] == 0x7f;
		_exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (child < 0) {
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	passed = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		 WEXITSTATUS(status) == EXIT_SUCCESS &&
		 check_slices(mapping.base, 0x51);
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

/*
 * A tuple folio may stay in the swapcache with tuple-level exclusivity while
 * some slices are still exclusive swap PTEs.  Zapping its only present slice
 * and forking must still turn the folio into a shared one: writes to swapped
 * slices in either process have to copy instead of landing in the folio the
 * other process reaches through its swap PTEs.
 */
static int fork_after_swapin_dontneed(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	unsigned char *slice1 = mapping.base + PROCESS_PAGE_SIZE;
	unsigned char *slice3 = mapping.base + 3 * PROCESS_PAGE_SIZE;
	int pipefd[2] = { -1, -1 };
	unsigned char token = 0;
	pid_t child;
	int status;
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x61);
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	/* An exclusive read swapin restores tuple-level exclusivity ... */
	passed = slice3[0] == 0x64;
	/* ... then the folio stays in the swapcache without a present slice. */
	passed = passed && !madvise(slice3, PROCESS_PAGE_SIZE, MADV_DONTNEED) &&
		 !pipe(pipefd);
	if (!passed) {
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	child = fork();
	if (!child) {
		close(pipefd[1]);
		/* Wait for the parent's write, which must not reach us. */
		passed = read(pipefd[0], &token, 1) == 1 && slice1[0] == 0x62;
		/* A non-exclusive swapin followed by a write must copy. */
		passed = passed && mapping.base[0] == 0x61;
		memset(mapping.base, 0x7e, PROCESS_PAGE_SIZE);
		passed = passed && mapping.base[0] == 0x7e &&
			 mapping.base[PROCESS_PAGE_SIZE - 1] == 0x7e &&
			 slice1[0] == 0x62 &&
			 mapping.base[2 * PROCESS_PAGE_SIZE] == 0x63 &&
			 slice3[0] == 0;
		_exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	close(pipefd[0]);
	if (child < 0) {
		close(pipefd[1]);
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	memset(slice1, 0x7d, PROCESS_PAGE_SIZE);
	passed = write(pipefd[1], &token, 1) == 1;
	close(pipefd[1]);
	passed = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		 WEXITSTATUS(status) == EXIT_SUCCESS && passed;
	/* The child's write must not have reached the parent either. */
	passed = passed && mapping.base[0] == 0x61 &&
		 mapping.base[PROCESS_PAGE_SIZE - 1] == 0x61 &&
		 slice1[0] == 0x7d &&
		 mapping.base[2 * PROCESS_PAGE_SIZE] == 0x63 && slice3[0] == 0;
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

/* Bit 63 of a /proc/self/pagemap entry: the PTE is present. */
static int slices_present(const unsigned char *base)
{
	uint64_t entry;
	int present = 0;
	unsigned int slice;

	for (slice = 0; slice < P3S_SLICES; slice++) {
		if (!p3s_pagemap_entry(base + slice * PROCESS_PAGE_SIZE,
				       &entry))
			return -1;
		if (entry >> 63)
			present++;
	}
	return present;
}

/*
 * A compat swapin may install only the faulting 4K PTE or more of the folio.
 * Either way, each slice must retain its own bytes.
 */
static int swapin_preserves_slice_data(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	unsigned int stage = 0;
	int pres;
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x31);
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	pres = slices_present(mapping.base);
	if (pres < 0) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	passed = pres == 0;
	if (passed)
		stage = 1;
	/* One read fault on slice 2 ... */
	passed = passed && mapping.base[2 * PROCESS_PAGE_SIZE] == 0x33;
	if (passed)
		stage = 2;
	/* Other slice PTEs may remain swapped until individually touched. */
	passed = passed && slices_present(mapping.base) >= 1;
	if (passed)
		stage = 3;
	passed = passed && check_slices(mapping.base, 0x31) &&
		slices_present(mapping.base) == P3S_SLICES;
	if (passed)
		stage = 4;
	if (!passed)
		ksft_print_msg("lazy slice swapin failed after stage %u\n",
			       stage);
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

static bool discard_refill_verify_range(unsigned char *base,
					unsigned int slice_bias)
{
	unsigned int tuple;

	for (tuple = 0; tuple < CHURN_TUPLES; tuple++) {
		unsigned char seed = 0x20 + tuple % 32;
		unsigned int slice = (tuple + slice_bias) % P3S_SLICES;
		unsigned char *tuple_base = base + tuple * NATIVE_PAGE_SIZE;

		if (madvise(tuple_base + slice * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			    MADV_DONTNEED))
			return false;
		memset(tuple_base + slice * PROCESS_PAGE_SIZE, seed + slice,
		       PROCESS_PAGE_SIZE);
		if (!check_slices(tuple_base, seed))
			return false;
	}
	return true;
}

static bool rewrite_verify_range(unsigned char *base, unsigned int slice_bias)
{
	unsigned int tuple;

	for (tuple = 0; tuple < CHURN_TUPLES; tuple++) {
		unsigned char seed = 0x20 + tuple % 32;
		unsigned int slice = (tuple + slice_bias) % P3S_SLICES;
		unsigned char *tuple_base = base + tuple * NATIVE_PAGE_SIZE;

		memset(tuple_base + slice * PROCESS_PAGE_SIZE, seed + slice,
		       PROCESS_PAGE_SIZE);
		if (!check_slices(tuple_base, seed))
			return false;
	}
	return true;
}

static int fork_before_pageout(void)
{
	const size_t length = CHURN_TUPLES * NATIVE_PAGE_SIZE;
	struct mapping mapping = map_tuples(length);
	struct test_cgroup cgroup = {};
	int ready[2] = { -1, -1 };
	int start[2] = { -1, -1 };
	unsigned int tuple;
	unsigned char byte = 1;
	pid_t child = -1;
	int status = 0;
	int parent_stage = 0;
	pid_t waited = -1;
	int ret = KSFT_FAIL;
	bool passed = true;

	if (mapping.base == MAP_FAILED || pipe(ready) || pipe(start))
		goto out;
	if (!enter_private_cgroup(&cgroup)) {
		ret = KSFT_SKIP;
		goto out;
	}
	for (tuple = 0; tuple < CHURN_TUPLES; tuple++)
		fill_slices(mapping.base + tuple * NATIVE_PAGE_SIZE,
			    0x20 + tuple % 32);

	child = fork();
	if (!child) {
		bool child_passed;

		close(ready[0]);
		close(start[1]);
		if (write(ready[1], &byte, 1) != 1 ||
		    read(start[0], &byte, 1) != 1)
			_exit(10);
		child_passed = rewrite_verify_range(mapping.base, 0);
		if (!child_passed)
			_exit(11);
		child_passed = discard_refill_verify_range(mapping.base, 2);
		_exit(child_passed ? EXIT_SUCCESS : 12);
	}
	if (child < 0)
		goto out;
	close(ready[1]);
	ready[1] = -1;
	close(start[0]);
	start[0] = -1;
	passed = read(ready[0], &byte, 1) == 1;
	if (passed)
		parent_stage = 1;
	if (passed && !cgroup_reclaim_reaches_swap(mapping.base, length,
						   length)) {
		close(start[1]);
		start[1] = -1;
		waitpid(child, &status, 0);
		ret = KSFT_SKIP;
		goto out;
	}
	if (passed)
		parent_stage = 2;
	if (write(start[1], &byte, 1) != 1)
		passed = false;
	else if (passed)
		parent_stage = 3;
	if (passed)
		passed = rewrite_verify_range(mapping.base, 1);
	if (passed)
		parent_stage = 4;
	if (passed)
		passed = discard_refill_verify_range(mapping.base, 3);
	if (passed)
		parent_stage = 5;
	waited = waitpid(child, &status, 0);
	passed = waited == child && WIFEXITED(status) &&
		 WEXITSTATUS(status) == EXIT_SUCCESS && passed;
	if (!passed)
		ksft_print_msg("pre-fork reclaim failed: parent_stage=%d "
			       "waited=%ld child_status=%#x\n", parent_stage,
			       (long)waited, status);
	ret = passed ? KSFT_PASS : KSFT_FAIL;
out:
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	if (start[0] >= 0)
		close(start[0]);
	if (start[1] >= 0)
		close(start[1]);
	unmap_mapping(&mapping);
	leave_private_cgroup(&cgroup);
	return ret;
}

static int fork_after_hole_fill(void)
{
	struct mapping mapping = map_tuples(NATIVE_PAGE_SIZE);
	pid_t child;
	int status;
	bool passed;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	fill_slices(mapping.base, 0x58);
	if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
				  NATIVE_PAGE_SIZE)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	if (madvise(mapping.base, PROCESS_PAGE_SIZE, MADV_DONTNEED)) {
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	memset(mapping.base, 0x58, PROCESS_PAGE_SIZE);
	child = fork();
	if (!child) {
		memset(mapping.base + 3 * PROCESS_PAGE_SIZE, 0x7f, PROCESS_PAGE_SIZE);
		/* PAGEOUT is advisory; no eviction means no swap COW coverage. */
		if (!pageout_reaches_swap(mapping.base, NATIVE_PAGE_SIZE,
					  2 * PROCESS_PAGE_SIZE))
			_exit(KSFT_SKIP);
		passed = mapping.base[0] == 0x58 &&
			 mapping.base[PROCESS_PAGE_SIZE] == 0x59 &&
			 mapping.base[2 * PROCESS_PAGE_SIZE] == 0x5a &&
			 mapping.base[3 * PROCESS_PAGE_SIZE] == 0x7f;
		_exit(passed ? KSFT_PASS : KSFT_FAIL);
	}
	if (child < 0) {
		unmap_mapping(&mapping);
		return KSFT_FAIL;
	}
	passed = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		 check_slices(mapping.base, 0x58);
	unmap_mapping(&mapping);
	return passed ? WEXITSTATUS(status) : KSFT_FAIL;
}

static void *swapin_worker(void *argument)
{
	struct worker *worker = argument;
	unsigned int tuple;

	for (tuple = 0; tuple < CONCURRENT_TUPLES; tuple++)
		memset(worker->base + tuple * NATIVE_PAGE_SIZE +
		       worker->slice * PROCESS_PAGE_SIZE,
		       0x70 + worker->slice, PROCESS_PAGE_SIZE);
	return NULL;
}

static int concurrent_swapin(void)
{
	const size_t length = CONCURRENT_TUPLES * NATIVE_PAGE_SIZE;
	struct mapping mapping = map_tuples(length);
	struct worker worker[P3S_SLICES];
	pthread_t thread[P3S_SLICES];
	unsigned int created = 0;
	unsigned int slice, tuple;
	bool passed = true;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	for (tuple = 0; tuple < CONCURRENT_TUPLES; tuple++)
		fill_slices(mapping.base + tuple * NATIVE_PAGE_SIZE, 0x61);
	if (!pageout_reaches_swap(mapping.base, length, length)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	for (slice = 0; passed && slice < P3S_SLICES; slice++) {
		worker[slice].base = mapping.base;
		worker[slice].slice = slice;
		if (pthread_create(&thread[slice], NULL, swapin_worker,
				   &worker[slice]))
			passed = false;
		else
			created++;
	}
	for (slice = 0; slice < created; slice++)
		if (pthread_join(thread[slice], NULL))
			passed = false;
	for (tuple = 0; passed && tuple < CONCURRENT_TUPLES; tuple++) {
		unsigned char *base = mapping.base + tuple * NATIVE_PAGE_SIZE;

		if (!check_slices(base, 0x70))
			passed = false;
	}
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

static int pageout_dontneed_churn(void)
{
	const size_t length = CHURN_TUPLES * NATIVE_PAGE_SIZE;
	struct mapping mapping = map_tuples(length);
	unsigned int round, tuple;
	unsigned int max_split = 0;
	unsigned int swapped_rounds = 0;
	bool passed = true;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	for (tuple = 0; tuple < CHURN_TUPLES; tuple++)
		fill_slices(mapping.base + tuple * NATIVE_PAGE_SIZE,
			    0x20 + tuple % 32);
	for (round = 0; passed && round < 8; round++) {
		unsigned int split = 0;

		passed = !madvise(mapping.base, length, MADV_PAGEOUT);
		if (passed && wait_for_swap(mapping.base, length,
					    PROCESS_PAGE_SIZE))
			swapped_rounds++;
		for (tuple = 0; passed && tuple < CHURN_TUPLES; tuple++) {
			unsigned char seed = 0x20 + tuple % 32;
			unsigned int slice = (tuple + round) % P3S_SLICES;
			unsigned char *base = mapping.base + tuple * NATIVE_PAGE_SIZE;

			if ((tuple + round) % 3 == 0) {
				passed = !madvise(base + slice * PROCESS_PAGE_SIZE,
						  PROCESS_PAGE_SIZE, MADV_DONTNEED);
				if (passed)
					memset(base + slice * PROCESS_PAGE_SIZE,
					       seed + slice, PROCESS_PAGE_SIZE);
			}
			if (passed && !check_slices(base, seed)) {
				ksft_print_msg("churn data mismatch at round %u tuple %u "
					       "bytes=%02x/%02x/%02x/%02x\n",
					       round, tuple, base[0],
					       base[PROCESS_PAGE_SIZE],
					       base[2 * PROCESS_PAGE_SIZE],
					       base[3 * PROCESS_PAGE_SIZE]);
				passed = false;
			}
			if (passed && !tuple_has_one_folio(base))
				split++;
		}
		if (split > max_split)
			max_split = split;
	}
	if (!passed)
		ksft_print_msg("churn failed at round %u tuple %u\n",
			       round, tuple);
	else
		ksft_print_msg("churn: %u/8 rounds swapped, peak split tuples %u/%u\n",
			       swapped_rounds, max_split, CHURN_TUPLES);
	unmap_mapping(&mapping);
	if (!passed)
		return KSFT_FAIL;
	return swapped_rounds == 8 ? KSFT_PASS : KSFT_SKIP;
}

static int compaction_preserves_tuples(void)
{
	const size_t length = COMPACT_TUPLES * NATIVE_PAGE_SIZE;
	struct mapping mapping = map_tuples(length);
	uint64_t pfn[P3S_SLICES];
	unsigned int tuple, attempt;
	bool passed = true;
	int fd;

	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	for (tuple = 0; tuple < COMPACT_TUPLES; tuple++)
		fill_slices(mapping.base + tuple * NATIVE_PAGE_SIZE,
			    0x30 + tuple % 32);
	if (!read_pfns(mapping.base, pfn) || !pfn[0]) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	fd = open("/proc/sys/vm/compact_memory", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	for (attempt = 0; attempt < 4; attempt++) {
		if (write(fd, "1", 1) != 1) {
			int err = errno;

			close(fd);
			unmap_mapping(&mapping);
			return (err == EPERM || err == EACCES) ?
				KSFT_SKIP : KSFT_FAIL;
		}
	}
	close(fd);
	for (tuple = 0; passed && tuple < COMPACT_TUPLES; tuple++) {
		unsigned char *base = mapping.base + tuple * NATIVE_PAGE_SIZE;

		passed = check_slices(base, 0x30 + tuple % 32) &&
			 tuple_has_one_folio(base);
	}
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

static int near_capacity_swap_pressure(void)
{
	const size_t length = PRESSURE_TUPLES * NATIVE_PAGE_SIZE;
	struct mapping mapping;
	unsigned long swapped = 0;
	unsigned int tuple;
	bool passed = true;

	if (swap_free_bytes() < MIN_PRESSURE_SWAP)
		return KSFT_SKIP;
	mapping = map_tuples(length);
	if (mapping.base == MAP_FAILED)
		return KSFT_FAIL;
	for (tuple = 0; tuple < PRESSURE_TUPLES; tuple++)
		fill_slices(mapping.base + tuple * NATIVE_PAGE_SIZE,
			    0x20 + tuple % 32);
	if (!pageout_reaches_swap(mapping.base, length, length / 2)) {
		unmap_mapping(&mapping);
		return KSFT_SKIP;
	}
	passed = p3s_smaps_bytes(mapping.base, length, "Swap", &swapped);
	for (tuple = 0; passed && tuple < PRESSURE_CHURN; tuple++) {
		unsigned int index = (tuple * 7919U) % PRESSURE_TUPLES;
		unsigned int slice = tuple % P3S_SLICES;
		unsigned char seed = 0x20 + index % 32;
		unsigned char *base = mapping.base + index * NATIVE_PAGE_SIZE;

		passed = check_slices(base, seed);
		if (!passed) {
			ksft_print_msg("swap pressure mismatch at iter %u "
				       "index %u bytes=%02x/%02x/%02x/%02x\n",
				       tuple, index, base[0],
				       base[PROCESS_PAGE_SIZE],
				       base[2 * PROCESS_PAGE_SIZE],
				       base[3 * PROCESS_PAGE_SIZE]);
			break;
		}
		passed = !madvise(base + slice * PROCESS_PAGE_SIZE,
				  PROCESS_PAGE_SIZE, MADV_DONTNEED);
		if (!passed)
			break;
		memset(base + slice * PROCESS_PAGE_SIZE, seed + slice,
		       PROCESS_PAGE_SIZE);
		if (!(tuple % 32) &&
		    madvise(base, NATIVE_PAGE_SIZE, MADV_PAGEOUT))
			passed = false;
	}
	for (tuple = 0; passed && tuple < PRESSURE_TUPLES; tuple++) {
		unsigned char *base = mapping.base + tuple * NATIVE_PAGE_SIZE;

		passed = check_slices(base, 0x20 + tuple % 32);
		if (!passed)
			ksft_print_msg("swap pressure final mismatch at %u "
				       "bytes=%02x/%02x/%02x/%02x\n", tuple,
				       base[0], base[PROCESS_PAGE_SIZE],
				       base[2 * PROCESS_PAGE_SIZE],
				       base[3 * PROCESS_PAGE_SIZE]);
	}
	if (!passed)
		ksft_print_msg("swap pressure failed at tuple %u "
			       "after swapping %lu bytes\n", tuple, swapped);
	unmap_mapping(&mapping);
	return passed ? KSFT_PASS : KSFT_FAIL;
}

#define RUN_SWAP_CASE(fn_call) do { \
	if (!p3s_swap_available()) \
		SKIP(return, "swap is unavailable"); \
	int result = (fn_call); \
	if (result == KSFT_SKIP) \
		SKIP(return, "precondition unavailable for swap test"); \
	EXPECT_EQ(KSFT_PASS, result); \
} while (0)

TEST(swap_first_swapin_write_fault)
{
	RUN_SWAP_CASE(first_swapin_is_write());
}

TEST(swap_partial_tuple_pageout)
{
	RUN_SWAP_CASE(partial_tuple_pageout());
}

TEST(swap_fork_swapped_tuple)
{
	RUN_SWAP_CASE(fork_swapped_tuple(false));
}

TEST(swap_fork_after_partial_swapin)
{
	RUN_SWAP_CASE(fork_swapped_tuple(true));
}

TEST(swap_fork_before_pageout)
{
	RUN_SWAP_CASE(fork_before_pageout());
}

TEST(swap_fork_after_hole_fill)
{
	RUN_SWAP_CASE(fork_after_hole_fill());
}

TEST(swap_concurrent_four_slice_swapin)
{
	RUN_SWAP_CASE(concurrent_swapin());
}

TEST(swap_pageout_dontneed_churn)
{
	RUN_SWAP_CASE(pageout_dontneed_churn());
}

TEST(swap_compaction_preserves_tuples)
{
	RUN_SWAP_CASE(compaction_preserves_tuples());
}

TEST(swap_near_capacity_swap_pressure)
{
	RUN_SWAP_CASE(near_capacity_swap_pressure());
}

TEST(swap_private_file_cow_swapin)
{
	RUN_SWAP_CASE(private_file_cow_swapin());
}

TEST(swap_read_swapin_write_pageout)
{
	RUN_SWAP_CASE(read_swapin_write_pageout());
}

TEST(swap_fork_after_swapin_dontneed)
{
	RUN_SWAP_CASE(fork_after_swapin_dontneed());
}

TEST(swap_swapin_preserves_slice_data)
{
	RUN_SWAP_CASE(swapin_preserves_slice_data());
}

P3S_HARNESS_4KB_MAIN
