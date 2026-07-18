// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/bpf.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define MAP_ENTRIES	4
#define MAP_SIZE	(MAP_ENTRIES * USER_PAGE_SIZE)

static int sys_bpf(enum bpf_cmd command, union bpf_attr *attr)
{
	return syscall(__NR_bpf, command, attr, sizeof(*attr));
}

static int create_array(void)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_ARRAY,
		.key_size = sizeof(unsigned int),
		.value_size = USER_PAGE_SIZE,
		.max_entries = MAP_ENTRIES,
		.map_flags = BPF_F_MMAPABLE,
	};

	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static bool initialize_array(int map_fd)
{
	unsigned char *value = malloc(USER_PAGE_SIZE);
	unsigned int key;
	bool success = true;

	if (!value)
		return false;
	for (key = 0; key < MAP_ENTRIES; key++) {
		union bpf_attr attr = {
			.map_fd = map_fd,
			.key = (unsigned long)&key,
			.value = (unsigned long)value,
			.flags = BPF_ANY,
		};

		memset(value, 0x41 + key, USER_PAGE_SIZE);
		if (sys_bpf(BPF_MAP_UPDATE_ELEM, &attr)) {
			success = false;
			break;
		}
	}
	free(value);
	return success;
}

static bool mapped_values_match(const unsigned char *mapping)
{
	unsigned int key;

	for (key = 0; key < MAP_ENTRIES; key++) {
		pid_t child = fork();
		int status;

		if (child < 0)
			return false;
		if (!child)
			_exit(mapping[key * USER_PAGE_SIZE] == 0x41 + key ? 0 : 1);
		if (waitpid(child, &status, 0) != child ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return false;
	}
	return true;
}

static int run_test(void)
{
	struct rlimit memlock = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	unsigned char *mapping;
	void *past_end;
	bool initialized;
	bool bounds_ok;
	bool values_ok;
	int map_fd;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	setrlimit(RLIMIT_MEMLOCK, &memlock);

	map_fd = create_array();
	if (map_fd < 0) {
		if (errno == EPERM || errno == EACCES)
			ksft_exit_skip("BPF map creation is unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("BPF_MAP_CREATE failed: %s\n",
				   strerror(errno));
	}
	ksft_test_result(true, "create a 16K mmapable BPF array\n");
	initialized = initialize_array(map_fd);
	ksft_test_result(initialized, "initialize all four 4K array values\n");
	if (!initialized)
		ksft_exit_fail_msg("BPF_MAP_UPDATE_ELEM failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       map_fd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("valid BPF array mmap failed: %s\n",
				   strerror(errno));
	values_ok = mapped_values_match(mapping);
	ksft_test_result(values_ok,
			 "every 4K slice maps the corresponding array value\n");

	errno = 0;
	past_end = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ, MAP_SHARED,
			map_fd, 3 * USER_PAGE_SIZE);
	bounds_ok = past_end == MAP_FAILED && errno == EINVAL;
	ksft_test_result(bounds_ok, "reject an mmap extending past the array\n");
	ksft_print_msg("out-of-bounds mmap=%p errno=%d\n", past_end, errno);

	if (past_end != MAP_FAILED)
		munmap(past_end, 2 * USER_PAGE_SIZE);
	munmap(mapping, MAP_SIZE);
	close(map_fd);
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
	execl("/proc/self/exe", "bpf_array_mmap_ppps", "--run", NULL);
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
