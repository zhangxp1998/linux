// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
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
#define NATIVE_PAGE_SIZE	16384UL
#define TARGET_OFFSET	(2 * USER_PAGE_SIZE)
#define REF_CTR_OFFSET	(3 * USER_PAGE_SIZE)
#define TARGET_MAP_ADDR	((void *)0x20001000UL)
#define REF_MAP_ADDR	((void *)0x30001000UL)
#define TARGET_PATH	"/tmp/uprobe-ppps-target"
#define TRACE_ROOT	"/sys/kernel/tracing"

typedef void (*target_fn_t)(void);

static bool write_text(const char *path, const char *text, int flags)
{
	ssize_t length = strlen(text);
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC | flags);
	if (fd < 0)
		return false;
	written = write(fd, text, length);
	close(fd);
	return written == length;
}

static void *map_target(void **ref_mapping)
{
#ifdef __aarch64__
	const uint32_t instructions[] = {
		0xd503201f, /* nop */
		0xd65f03c0, /* ret */
	};
	void *mapping = MAP_FAILED;
	int fd;

	*ref_mapping = MAP_FAILED;
	fd = open(TARGET_PATH, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
	if (fd < 0)
		return MAP_FAILED;
	if (ftruncate(fd, NATIVE_PAGE_SIZE) ||
	    pwrite(fd, instructions, sizeof(instructions), TARGET_OFFSET) !=
		    sizeof(instructions))
		goto out;
	mapping = mmap(TARGET_MAP_ADDR, NATIVE_PAGE_SIZE, PROT_READ | PROT_EXEC,
		       MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (mapping == MAP_FAILED)
		goto out;
	*ref_mapping = mmap(REF_MAP_ADDR, NATIVE_PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (*ref_mapping == MAP_FAILED) {
		munmap(mapping, NATIVE_PAGE_SIZE);
		mapping = MAP_FAILED;
	}
out:
	close(fd);
	return mapping;
#else
	return MAP_FAILED;
#endif
}

static bool trace_has_event(void)
{
	char buffer[65536];
	ssize_t size;
	int fd;

	fd = open(TRACE_ROOT "/trace", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	size = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (size < 0)
		return false;
	buffer[size] = '\0';
	return strstr(buffer, "uprobe_ppps:");
}

static int run_test(void)
{
#ifdef __aarch64__
	const char *delete_command = "-:ppps/uprobe_ppps\n";
	const char *create_command =
		"p:ppps/uprobe_ppps " TARGET_PATH ":0x2000(0x3000)\n";
	uint16_t *ref_ctr;
	target_fn_t target;
	void *ref_mapping;
	void *mapping;
	bool registered;
	bool unregistered;
	bool observed;
	unsigned int i;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	mapping = map_target(&ref_mapping);
	ksft_test_result(mapping == TARGET_MAP_ADDR &&
			 ref_mapping == REF_MAP_ADDR,
			 "map text and ref-counter VMAs one 4K slice past native alignment\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("target mapping failed: %s\n",
				   strerror(errno));
	target = (target_fn_t)((char *)mapping + TARGET_OFFSET);
	ref_ctr = (uint16_t *)((char *)ref_mapping + REF_CTR_OFFSET);
	ksft_print_msg("mapping=%p target=%p native_slice=%lu\n", mapping,
		       target, TARGET_OFFSET / USER_PAGE_SIZE);

	for (i = 0; i < 1024; i++)
		target();
	ksft_test_result(true, "warm the target instruction and translation\n");

	write_text(TRACE_ROOT "/uprobe_events", delete_command, O_APPEND);
	write_text(TRACE_ROOT "/trace", "", O_TRUNC);
	registered = write_text(TRACE_ROOT "/uprobe_events", create_command,
				O_APPEND) &&
		write_text(TRACE_ROOT "/events/ppps/uprobe_ppps/enable", "1", 0) &&
		write_text(TRACE_ROOT "/tracing_on", "1", 0);
	ksft_test_result(registered, "register and enable the sliced uprobe\n");
	if (!registered)
		ksft_exit_fail_msg("uprobe registration failed: %s\n",
				   strerror(errno));
	ksft_test_result(*ref_ctr == 1,
			 "increment the sliced userspace reference counter\n");

	target();
	ksft_test_result(true, "execute and return from the probed function\n");
	write_text(TRACE_ROOT "/events/ppps/uprobe_ppps/enable", "0", 0);
	observed = trace_has_event();
	ksft_test_result(observed, "record an event from the sliced uprobe\n");
	unregistered = write_text(TRACE_ROOT "/uprobe_events", delete_command,
				  O_APPEND);
	ksft_test_result(unregistered, "unregister the sliced uprobe\n");
	ksft_test_result(*ref_ctr == 0,
			 "decrement the sliced userspace reference counter\n");

	munmap(mapping, NATIVE_PAGE_SIZE);
	munmap(ref_mapping, NATIVE_PAGE_SIZE);
	unlink(TARGET_PATH);
	ksft_finished();
#else
	ksft_exit_skip("PPPS uprobes are arm64-specific\n");
#endif
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
	execl("/proc/self/exe", "uprobe_ppps", "--run", NULL);
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
