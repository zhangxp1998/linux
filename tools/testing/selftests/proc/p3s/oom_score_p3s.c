// SPDX-License-Identifier: GPL-2.0
/*
 * Faulting anonymous or shmem mappings in a 4KB process raises its VmRSS
 * (and memcg "anon" charge) by the faulted byte count and scales oom_score
 * with resident bytes rather than 4KB page count.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define MIN_MAP_SIZE (32UL * 1024 * 1024)

static long read_meminfo_kb(const char *name)
{
	char line[256];
	size_t name_len = strlen(name);
	FILE *file;
	long value = -1;

	file = fopen("/proc/meminfo", "re");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, name, name_len) && line[name_len] == ':' &&
		    sscanf(line + name_len + 1, " %ld kB", &value) == 1)
			break;
		value = -1;
	}
	fclose(file);
	return value;
}

static long read_oom_score(void)
{
	FILE *file;
	long score = -1;

	file = fopen("/proc/self/oom_score", "re");
	if (!file)
		return -1;
	if (fscanf(file, "%ld", &score) != 1)
		score = -1;
	fclose(file);
	return score;
}

static bool current_cgroup_file(char *path, size_t size, const char *file)
{
	char line[PATH_MAX];
	char *newline;
	FILE *cgroup;
	int length;

	cgroup = fopen("/proc/self/cgroup", "re");
	if (!cgroup)
		return false;
	while (fgets(line, sizeof(line), cgroup)) {
		if (strncmp(line, "0::", 3))
			continue;
		newline = strchr(line + 3, '\n');
		if (newline)
			*newline = '\0';
		length = snprintf(path, size, "/sys/fs/cgroup%s/%s", line + 3,
				  file);
		if (length < 0 || (size_t)length >= size) {
			fclose(cgroup);
			return false;
		}
		fclose(cgroup);
		return true;
	}
	fclose(cgroup);
	return false;
}

static long read_memcg_anon_kb(void)
{
	char path[PATH_MAX];
	char name[64];
	unsigned long bytes;
	FILE *stat;

	if (!current_cgroup_file(path, sizeof(path), "memory.stat"))
		return -1;
	stat = fopen(path, "re");
	if (!stat)
		return -1;
	while (fscanf(stat, "%63s %lu", name, &bytes) == 2) {
		if (!strcmp(name, "anon")) {
			fclose(stat);
			return bytes / 1024;
		}
	}
	fclose(stat);
	return -1;
}

static bool select_probe_size(long *memtotal, long *swaptotal,
			      size_t *map_size)
{
	unsigned long long total_bytes, target_bytes;
	long memavail;

	*memtotal = read_meminfo_kb("MemTotal");
	*swaptotal = read_meminfo_kb("SwapTotal");
	if (*swaptotal < 0)
		*swaptotal = 0;
	if (*memtotal <= 0)
		return false;
	total_bytes = (unsigned long long)(*memtotal + *swaptotal) * 1024ULL;
	target_bytes = total_bytes * 9ULL / 400ULL;
	if (target_bytes < MIN_MAP_SIZE)
		target_bytes = MIN_MAP_SIZE;
	target_bytes = (target_bytes + NATIVE_PAGE_SIZE - 1) &
		       ~(unsigned long long)(NATIVE_PAGE_SIZE - 1);
	if (target_bytes > SIZE_MAX)
		return false;
	*map_size = (size_t)target_bytes;

	memavail = read_meminfo_kb("MemAvailable");
	if (memavail >= 0 &&
	    (unsigned long long)memavail * 1024ULL <
	    (unsigned long long)*map_size + *map_size / 4)
		return false;
	return true;
}

static bool score_in_range(long score_delta, long delta_kb, long total_kb,
			   long *expected_out, long *tol_out)
{
	long expected = delta_kb * 2000 / (3 * total_kb);
	long tolerance = expected / 3;

	if (tolerance < 3)
		tolerance = 3;
	*expected_out = expected;
	*tol_out = tolerance;
	return expected > 0 && score_delta > 0 &&
	       labs(score_delta - expected) <= tolerance;
}

TEST(oom_anon_score_scales_with_bytes)
{
	long anon_before, anon_after, anon_delta;
	long rss_before, rss_after, rss_delta;
	long score_before, score_after, score_delta;
	long memtotal, swaptotal, expected_delta, tolerance, map_kb;
	unsigned char *mapping;
	size_t map_size, offset;
	bool score_matches;

	/* Warm stdio and cgroup rstat paths before taking the baseline. */
	read_memcg_anon_kb();
	read_oom_score();
	p3s_status_kb("VmRSS");

	if (!select_probe_size(&memtotal, &swaptotal, &map_size))
		SKIP(return, "insufficient memory for OOM score probe");
	anon_before = read_memcg_anon_kb();
	rss_before = p3s_status_kb("VmRSS");
	score_before = read_oom_score();
	if (anon_before < 0 || rss_before < 0 || score_before < 0)
		SKIP(return, "memory cgroup v2 accounting is unavailable");

	mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		if (errno == ENOMEM)
			SKIP(return, "insufficient memory for OOM probe");
		ASSERT_NE(MAP_FAILED, mapping);
	}
	madvise(mapping, map_size, MADV_NOHUGEPAGE);
	for (offset = 0; offset < map_size; offset += PROCESS_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset;

	anon_after = read_memcg_anon_kb();
	rss_after = p3s_status_kb("VmRSS");
	score_after = read_oom_score();
	anon_delta = anon_after - anon_before;
	rss_delta = rss_after - rss_before;
	score_delta = score_after - score_before;
	map_kb = (long)(map_size / 1024);

	EXPECT_TRUE(rss_delta >= map_kb * 7 / 8 &&
		    labs(rss_delta - anon_delta) <=
		    (anon_delta / 8 > 8192 ? anon_delta / 8 : 8192));
	EXPECT_TRUE(anon_delta >= map_kb * 7 / 8);

	score_matches = score_in_range(score_delta, anon_delta,
				       memtotal + swaptotal, &expected_delta,
				       &tolerance);
	ksft_print_msg("mem=%ldkB swap=%ldkB rss=%ldkB anon=%ldkB score=%ld\n",
		       memtotal, swaptotal, rss_delta, anon_delta, score_delta);
	ksft_print_msg("expected=%ld tolerance=%ld\n",
		       expected_delta, tolerance);
	EXPECT_TRUE(score_matches);

	munmap(mapping, map_size);
}

TEST(oom_shmem_score_scales_with_bytes)
{
	long rss_before, rss_after, rss_delta;
	long score_before, score_after, score_delta;
	long memtotal, swaptotal, expected_delta, tolerance, map_kb;
	unsigned char *mapping;
	size_t map_size, offset;
	bool score_matches;
	int fd;

	if (!select_probe_size(&memtotal, &swaptotal, &map_size))
		SKIP(return, "insufficient memory for OOM score probe");
	rss_before = p3s_status_kb("VmRSS");
	score_before = read_oom_score();
	ASSERT_GE(rss_before, 0);
	ASSERT_GE(score_before, 0);

	fd = memfd_create("oom-score-p3s", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	if (ftruncate(fd, map_size)) {
		int err = errno;

		close(fd);
		if (err == ENOMEM || err == ENOSPC)
			SKIP(return, "insufficient shmem space for OOM probe");
		errno = err;
		ASSERT_EQ(0, -1);
	}
	mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		int err = errno;

		close(fd);
		if (err == ENOMEM)
			SKIP(return, "insufficient memory for OOM probe");
		errno = err;
		ASSERT_NE(MAP_FAILED, mapping);
	}
	for (offset = 0; offset < map_size; offset += PROCESS_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset;

	rss_after = p3s_status_kb("VmRSS");
	score_after = read_oom_score();
	rss_delta = rss_after - rss_before;
	score_delta = score_after - score_before;
	map_kb = (long)(map_size / 1024);

	EXPECT_TRUE(rss_delta >= map_kb * 7 / 8 &&
		    rss_delta <= map_kb + map_kb / 8);

	score_matches = score_in_range(score_delta, rss_delta,
				       memtotal + swaptotal, &expected_delta,
				       &tolerance);
	ksft_print_msg("mem=%ldkB swap=%ldkB rss=%ld/%ldkB score=%ld/%ld\n",
		       memtotal, swaptotal, rss_before, rss_after,
		       score_before, score_after);
	ksft_print_msg("delta=%ld expected=%ld tolerance=%ld\n",
		       score_delta, expected_delta, tolerance);
	EXPECT_TRUE(score_matches);

	munmap(mapping, map_size);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
