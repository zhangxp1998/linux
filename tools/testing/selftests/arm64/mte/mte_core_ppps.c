// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <asm/hwcap.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#ifndef PROT_MTE
#define PROT_MTE 0x20
#endif

#ifndef PT_AARCH64_MEMTAG_MTE
#define PT_AARCH64_MEMTAG_MTE (PT_LOPROC + 0x2)
#endif

#define USER_PAGE_SIZE 4096UL
#define MTE_GRANULE_SIZE 16UL
#define MTE_TAG_BITS 4UL
#define TEST_PAGES 4UL
#define TEST_SIZE (TEST_PAGES * USER_PAGE_SIZE)
#define TAG_BYTES_PER_PAGE \
	(USER_PAGE_SIZE * MTE_TAG_BITS / (MTE_GRANULE_SIZE * 8))
#define TOTAL_TAG_BYTES (TEST_PAGES * TAG_BYTES_PER_PAGE)
#define TEST_BASE 0x50000000UL
#define CORE_PATTERN "/tmp/mte-core-ppps-%p"

static bool write_file(const char *path, const char *contents, size_t length)
{
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	written = write(fd, contents, length);
	close(fd);
	return written == (ssize_t)length;
}

static char *read_core_pattern(size_t *length)
{
	char *pattern = NULL;
	size_t capacity = 0;
	ssize_t size;
	FILE *file;

	file = fopen("/proc/sys/kernel/core_pattern", "re");
	if (!file)
		return NULL;
	size = getline(&pattern, &capacity, file);
	fclose(file);
	if (size < 0) {
		free(pattern);
		return NULL;
	}
	*length = size;
	return pattern;
}

static void store_allocation_tag(void *address, unsigned int tag)
{
	uintptr_t tagged = (uintptr_t)address | ((uintptr_t)tag << 56);

	asm volatile("stg %0, [%0]" : : "r" (tagged) : "memory");
}

static void crash_with_mte_mapping(void)
{
	struct rlimit limit = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	unsigned long offset;
	void *mapping;

	if (setrlimit(RLIMIT_CORE, &limit) ||
	    !write_file("/proc/self/coredump_filter", "0x1\n", 4))
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
	raise(SIGSEGV);
	_exit(103);
}

static bool inspect_core(const char *path, bool *segment_found,
			 uint64_t *segment_size, bool *tags_match)
{
	Elf64_Ehdr header;
	Elf64_Phdr phdr;
	unsigned int index;
	int fd;

	*segment_found = false;
	*segment_size = 0;
	*tags_match = false;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (read(fd, &header, sizeof(header)) != sizeof(header) ||
	    memcmp(header.e_ident, ELFMAG, SELFMAG) ||
	    header.e_phentsize != sizeof(phdr))
		goto error;
	for (index = 0; index < header.e_phnum; index++) {
		unsigned char tags[TOTAL_TAG_BYTES];
		unsigned long byte;

		if (pread(fd, &phdr, sizeof(phdr),
			  header.e_phoff + index * sizeof(phdr)) != sizeof(phdr))
			goto error;
		if (phdr.p_type != PT_AARCH64_MEMTAG_MTE ||
		    phdr.p_vaddr != TEST_BASE)
			continue;
		*segment_found = true;
		*segment_size = phdr.p_filesz;
		if (phdr.p_filesz != sizeof(tags) ||
		    pread(fd, tags, sizeof(tags), phdr.p_offset) != sizeof(tags))
			break;
		*tags_match = true;
		for (byte = 0; byte < sizeof(tags); byte++) {
			unsigned int page = byte / TAG_BYTES_PER_PAGE;
			unsigned char expected = (page + 1) * 0x11;

			if (tags[byte] == expected)
				continue;
			ksft_print_msg("tag byte %lu is %#x, expected %#x\n",
				       byte, tags[byte], expected);
			*tags_match = false;
			break;
		}
		break;
	}
	close(fd);
	return true;

error:
	close(fd);
	return false;
}

static int run_test(void)
{
	char core_path[128];
	char *old_pattern;
	size_t old_pattern_length = 0;
	uint64_t segment_size = 0;
	bool inspected;
	bool segment_found = false;
	bool tags_match = false;
	int status;
	pid_t child;

	ksft_print_header();
	if (!(getauxval(AT_HWCAP2) & HWCAP2_MTE))
		ksft_exit_skip("MTE is not supported\n");
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	old_pattern = read_core_pattern(&old_pattern_length);
	if (!old_pattern)
		ksft_exit_fail_msg("read core_pattern failed: %s\n",
				   strerror(errno));
	if (!write_file("/proc/sys/kernel/core_pattern", CORE_PATTERN,
			strlen(CORE_PATTERN))) {
		free(old_pattern);
		ksft_exit_skip("cannot install a temporary core pattern\n");
	}
	ksft_test_result_pass("install a temporary core pattern\n");

	child = fork();
	if (!child)
		crash_with_mte_mapping();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid failed: %s\n", strerror(errno));
	ksft_test_result(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
			 "generate a core from the tagged 4K process\n");

	snprintf(core_path, sizeof(core_path), "/tmp/mte-core-ppps-%d", child);
	inspected = inspect_core(core_path, &segment_found, &segment_size,
				 &tags_match);
	ksft_test_result(inspected, "parse the generated ELF core\n");
	ksft_test_result(inspected && segment_found,
			 "find the tagged mapping's MTE segment\n");
	ksft_test_result(inspected && segment_found &&
			 segment_size == TOTAL_TAG_BYTES,
			 "encode the MTE segment at process-page granularity\n");
	ksft_test_result(inspected && segment_found && tags_match,
			 "preserve tags across each 4K process page\n");

	unlink(core_path);
	if (!write_file("/proc/sys/kernel/core_pattern", old_pattern,
			old_pattern_length))
		ksft_print_msg("warning: could not restore core_pattern\n");
	free(old_pattern);
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
	execl("/proc/self/exe", "mte_core_ppps", "--run", NULL);
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
