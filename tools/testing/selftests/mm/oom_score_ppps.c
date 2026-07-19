// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define MAP_SIZE (128UL * 1024 * 1024)

static long read_named_kb(const char *path, const char *name)
{
	char line[256];
	FILE *file;
	long value = -1;

	file = fopen(path, "re");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%*[^:]: %ld kB", &value) == 1 &&
		    !strncmp(line, name, strlen(name)))
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

static int run_test(void)
{
	unsigned char *mapping;
	long rss_before, rss_after, rss_delta;
	long score_before, score_after, score_delta;
	long memtotal, expected_delta, tolerance;
	bool score_matches;
	size_t offset;
	int fd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	memtotal = read_named_kb("/proc/meminfo", "MemTotal");
	rss_before = read_named_kb("/proc/self/status", "VmRSS");
	score_before = read_oom_score();
	fd = memfd_create("oom-score-ppps", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, MAP_SIZE))
		ksft_exit_fail_msg("create shmem file failed: %s\n",
				   strerror(errno));
	mapping = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("map shmem file failed: %s\n", strerror(errno));
	for (offset = 0; offset < MAP_SIZE; offset += USER_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset;

	rss_after = read_named_kb("/proc/self/status", "VmRSS");
	score_after = read_oom_score();
	rss_delta = rss_after - rss_before;
	score_delta = score_after - score_before;
	ksft_test_result(rss_delta > 120 * 1024 && rss_delta < 136 * 1024,
			 "fault 128 MiB of shared memory into RSS\n");
	ksft_test_result(score_delta > 0, "increase oom_score with resident memory\n");

	expected_delta = rss_delta * 2000 / (3 * memtotal);
	tolerance = expected_delta / 3;
	if (tolerance < 8)
		tolerance = 8;
	score_matches = memtotal > 0 && score_before >= 0 && score_after >= 0 &&
		labs(score_delta - expected_delta) <= tolerance;
	ksft_print_msg("total=%ldkB rss=%ld/%ldkB score=%ld/%ld delta=%ld expected=%ld tol=%ld\n",
		       memtotal, rss_before, rss_after, score_before, score_after,
		       score_delta, expected_delta, tolerance);
	ksft_test_result(score_matches,
			 "scale oom_score by resident bytes, not process page count\n");

	munmap(mapping, MAP_SIZE);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n", strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n", strerror(errno));
	execl("/proc/self/exe", "oom_score_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	(void)argv;
	if (sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE)
		return run_test();
	if (argc == 1)
		return exec_compat();
	ksft_exit_skip("4K compatibility process is unavailable\n");
}
