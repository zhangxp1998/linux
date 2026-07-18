// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
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

#define USER_PAGE_SIZE	4096UL
#define TRACEFS		"/sys/kernel/tracing/"

static bool write_control(const char *name, const char *value)
{
	char path[256];
	ssize_t length = strlen(value);
	int fd;

	snprintf(path, sizeof(path), TRACEFS "%s", name);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (write(fd, value, length) != length) {
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static bool install_probe(void)
{
	write_control("tracing_on", "0\n");
	write_control("kprobe_events", "-:ppps_msync\n");
	if (!write_control("kprobe_events",
			   "p:ppps_msync vfs_fsync_range start=%x1 end=%x2\n"))
		return false;
	if (!write_control("events/kprobes/ppps_msync/enable", "1\n"))
		return false;
	if (!write_control("trace", "\n"))
		return false;
	return write_control("tracing_on", "1\n");
}

static void remove_probe(void)
{
	write_control("tracing_on", "0\n");
	write_control("events/kprobes/ppps_msync/enable", "0\n");
	write_control("kprobe_events", "-:ppps_msync\n");
}

static bool read_fsync_range(unsigned long long *start,
			     unsigned long long *end)
{
	char line[1024];
	bool found = false;
	FILE *trace = fopen(TRACEFS "trace", "r");

	if (!trace)
		return false;
	while (fgets(line, sizeof(line), trace)) {
		char *range;

		if (!strstr(line, "ppps_msync:"))
			continue;
		range = strstr(line, "start=0x");
		if (range && sscanf(range, "start=0x%llx end=0x%llx",
				    start, end) == 2)
			found = true;
	}
	fclose(trace);
	return found;
}

static int run_test(void)
{
	unsigned long long fsync_start = 0;
	unsigned long long fsync_end = 0;
	unsigned char *mapping;
	bool range_ok;
	bool probe_ok;
	bool sync_ok;
	int memfd;

	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	memfd = memfd_create("msync-offset-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, 4 * USER_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       memfd, USER_PAGE_SIZE);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	mapping[0] = 0x4d;

	probe_ok = install_probe();
	ksft_test_result(probe_ok, "trace the fsync range used by msync\n");
	if (!probe_ok)
		ksft_exit_skip("kprobe tracing is unavailable: %s\n",
			       strerror(errno));

	sync_ok = !msync(mapping, USER_PAGE_SIZE, MS_SYNC);
	write_control("tracing_on", "0\n");
	ksft_test_result(sync_ok, "synchronize the offset shmem mapping\n");
	range_ok = read_fsync_range(&fsync_start, &fsync_end) &&
		   fsync_start == USER_PAGE_SIZE &&
		   fsync_end == 2 * USER_PAGE_SIZE - 1;
	ksft_test_result(range_ok, "msync uses the PPPS byte file offset\n");
	ksft_print_msg("vfs_fsync_range start=%llu end=%llu\n",
		       fsync_start, fsync_end);

	remove_probe();
	munmap(mapping, USER_PAGE_SIZE);
	close(memfd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "msync_offset_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
