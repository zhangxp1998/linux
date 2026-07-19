// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <asm/hwcap.h>
#include <errno.h>
#include <linux/memfd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef PROT_MTE
#define PROT_MTE 0x20
#endif

#ifndef PTRACE_PEEKMTETAGS
#define PTRACE_PEEKMTETAGS 33
#endif

#ifndef PTRACE_POKEMTETAGS
#define PTRACE_POKEMTETAGS 34
#endif

#define USER_PAGE_SIZE 4096UL
#define MTE_GRANULE_SIZE 16UL
#define TEST_PAGES 4UL
#define TEST_SIZE (TEST_PAGES * USER_PAGE_SIZE)
#define TAGS_PER_PAGE (USER_PAGE_SIZE / MTE_GRANULE_SIZE)
#define TOTAL_TAGS (TEST_SIZE / MTE_GRANULE_SIZE)
#define TEST_BASE 0x50000000UL
#define FILE_TEST_BASE 0x60000000UL
#define FILE_TAG 5U
#define FILE_REPLACEMENT_TAG 13U

static void store_allocation_tag(void *address, unsigned int tag)
{
	uintptr_t tagged = (uintptr_t)address | ((uintptr_t)tag << 56);

	asm volatile("stg %0, [%0]" : : "r" (tagged) : "memory");
}

static unsigned int load_allocation_tag(void *address)
{
	uintptr_t tagged;

	asm volatile("ldg %0, [%1]" : "=r" (tagged) : "r" (address));
	return (tagged >> 56) & 0xf;
}

static void tracee(void)
{
	unsigned long offset;
	void *file_mapping;
	void *mapping;
	int fd;

	if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
		_exit(100);
	if (prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE, 0, 0, 0))
		_exit(101);
	mapping = mmap((void *)TEST_BASE, TEST_SIZE,
		       PROT_READ | PROT_WRITE | PROT_MTE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (mapping != (void *)TEST_BASE)
		_exit(102);
	memset(mapping, 0, TEST_SIZE);
	for (offset = 0; offset < TEST_SIZE; offset += MTE_GRANULE_SIZE) {
		unsigned int page = offset / USER_PAGE_SIZE;

		store_allocation_tag(mapping + offset, page + 1);
	}
	fd = memfd_create("mte-ptrace-ppps", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, 2 * USER_PAGE_SIZE))
		_exit(103);
	file_mapping = mmap((void *)FILE_TEST_BASE, USER_PAGE_SIZE,
			    PROT_READ | PROT_WRITE | PROT_MTE,
			    MAP_SHARED | MAP_FIXED_NOREPLACE, fd, USER_PAGE_SIZE);
	close(fd);
	if (file_mapping != (void *)FILE_TEST_BASE)
		_exit(104);
	memset(file_mapping, 0, USER_PAGE_SIZE);
	for (offset = 0; offset < USER_PAGE_SIZE; offset += MTE_GRANULE_SIZE)
		store_allocation_tag(file_mapping + offset, FILE_TAG);
	raise(SIGSTOP);
	for (offset = 0; offset < TEST_SIZE; offset += MTE_GRANULE_SIZE) {
		unsigned int page = offset / USER_PAGE_SIZE;

		if (load_allocation_tag(mapping + offset) != page + 8)
			_exit(105);
	}
	for (offset = 0; offset < USER_PAGE_SIZE; offset += MTE_GRANULE_SIZE)
		if (load_allocation_tag(file_mapping + offset) !=
		    FILE_REPLACEMENT_TAG)
			_exit(106);
	_exit(0);
}

static int run_test(void)
{
	unsigned char tags[TOTAL_TAGS];
	struct iovec iov = {
		.iov_base = tags,
		.iov_len = sizeof(tags),
	};
	unsigned long index;
	bool child_stopped;
	bool contents_ok = true;
	bool file_contents_ok = true;
	bool file_peek_ok;
	bool file_poke_ok;
	bool peek_ok;
	bool poke_ok;
	int status;
	pid_t child;

	ksft_print_header();
	if (!(getauxval(AT_HWCAP2) & HWCAP2_MTE))
		ksft_exit_skip("MTE is not supported\n");
	ksft_set_plan(10);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	child = fork();
	if (!child)
		tracee();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	child_stopped = WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP;
	ksft_test_result(child_stopped,
			 "tracee creates tagged anonymous and file-backed mappings\n");
	if (!child_stopped) {
		ksft_test_result_fail("read allocation tags with ptrace\n");
		ksft_test_result_fail("preserve tags across each 4K process page\n");
		ksft_test_result_fail("write allocation tags with ptrace\n");
		ksft_test_result_fail("read tags from a file-backed 4K slice\n");
		ksft_test_result_fail("preserve the file-backed slice's tags\n");
		ksft_test_result_fail("write tags to a file-backed 4K slice\n");
		ksft_test_result_fail("tracee observes tags written to every anonymous page\n");
		ksft_test_result_fail("tracee observes tags written to the file-backed slice\n");
		ksft_finished();
	}

	memset(tags, 0xff, sizeof(tags));
	peek_ok = !ptrace((enum __ptrace_request)PTRACE_PEEKMTETAGS, child,
			  (void *)TEST_BASE, &iov) && iov.iov_len == TOTAL_TAGS;
	ksft_test_result(peek_ok, "read allocation tags with ptrace\n");
	if (peek_ok) {
		for (index = 0; index < TOTAL_TAGS; index++) {
			unsigned char expected = index / TAGS_PER_PAGE + 1;

			if (tags[index] == expected)
				continue;
			ksft_print_msg("tag %lu is %u, expected %u\n",
				       index, tags[index], expected);
			contents_ok = false;
			break;
		}
	} else {
		contents_ok = false;
	}
	ksft_test_result(contents_ok,
			 "preserve tags across each 4K process page\n");

	for (index = 0; index < TOTAL_TAGS; index++)
		tags[index] = index / TAGS_PER_PAGE + 8;
	iov.iov_len = sizeof(tags);
	poke_ok = !ptrace((enum __ptrace_request)PTRACE_POKEMTETAGS, child,
			  (void *)TEST_BASE, &iov) && iov.iov_len == TOTAL_TAGS;
	ksft_test_result(poke_ok, "write allocation tags with ptrace\n");

	memset(tags, 0xff, TAGS_PER_PAGE);
	iov.iov_len = TAGS_PER_PAGE;
	file_peek_ok = !ptrace((enum __ptrace_request)PTRACE_PEEKMTETAGS,
			       child, (void *)FILE_TEST_BASE, &iov) &&
			iov.iov_len == TAGS_PER_PAGE;
	ksft_test_result(file_peek_ok,
			 "read tags from a file-backed 4K slice\n");
	if (file_peek_ok) {
		for (index = 0; index < TAGS_PER_PAGE; index++) {
			if (tags[index] == FILE_TAG)
				continue;
			ksft_print_msg("file tag %lu is %u, expected %u\n",
				       index, tags[index], FILE_TAG);
			file_contents_ok = false;
			break;
		}
	} else {
		file_contents_ok = false;
	}
	ksft_test_result(file_contents_ok,
			 "preserve the file-backed slice's tags\n");

	memset(tags, FILE_REPLACEMENT_TAG, TAGS_PER_PAGE);
	iov.iov_len = TAGS_PER_PAGE;
	file_poke_ok = !ptrace((enum __ptrace_request)PTRACE_POKEMTETAGS,
			       child, (void *)FILE_TEST_BASE, &iov) &&
			iov.iov_len == TAGS_PER_PAGE;
	ksft_test_result(file_poke_ok,
			 "write tags to a file-backed 4K slice\n");

	if (ptrace(PTRACE_CONT, child, NULL, NULL))
		kill(child, SIGKILL);
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("final waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) &&
			 (!WEXITSTATUS(status) || WEXITSTATUS(status) == 106),
			 "tracee observes tags written to every anonymous page\n");
	ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
			 "tracee observes tags written to the file-backed slice\n");
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
	execl("/proc/self/exe", "mte_ptrace_ppps", "--run", NULL);
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
