// SPDX-License-Identifier: GPL-2.0
/*
 * An ELF core dumped by a 4K compat process keeps its 4K VMAs aligned and
 * intact in PT_LOAD, omits a mapping that starts at file offset 4K, and
 * encodes 4K page units in NT_FILE.
 */
#define _GNU_SOURCE

#include <elf.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define TEST_VMAS	4
#define TEST_BASE	0x50000000UL
#define TEST_STRIDE	0x10000UL
#define CORE_PATTERN	"/tmp/elf-core-ppps-%p"

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

static void crash_with_test_vmas(void)
{
	struct rlimit limit = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	unsigned int index;

	if (setrlimit(RLIMIT_CORE, &limit))
		_exit(120);
	for (index = 0; index < TEST_VMAS; index++) {
		void *target = (void *)(TEST_BASE + index * TEST_STRIDE);
		void *mapping;

		mapping = mmap(target, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			       -1, 0);
		if (mapping != target)
			_exit(121);
		memset(mapping, 0x40 + index, USER_PAGE_SIZE);
	}
	raise(SIGSEGV);
	_exit(122);
}

static bool inspect_core(const char *path, unsigned int *found,
			 unsigned int *misaligned)
{
	Elf64_Ehdr header;
	Elf64_Phdr phdr;
	unsigned int index;
	int fd;

	*found = 0;
	*misaligned = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (read(fd, &header, sizeof(header)) != sizeof(header) ||
	    memcmp(header.e_ident, ELFMAG, SELFMAG) ||
	    header.e_phentsize != sizeof(phdr))
		goto error;
	for (index = 0; index < header.e_phnum; index++) {
		unsigned long target_index;

		if (pread(fd, &phdr, sizeof(phdr),
			  header.e_phoff + index * sizeof(phdr)) != sizeof(phdr))
			goto error;
		if (phdr.p_type != PT_LOAD || phdr.p_vaddr < TEST_BASE ||
		    phdr.p_vaddr >= TEST_BASE + TEST_VMAS * TEST_STRIDE)
			continue;
		target_index = (phdr.p_vaddr - TEST_BASE) / TEST_STRIDE;
		if (phdr.p_vaddr != TEST_BASE + target_index * TEST_STRIDE)
			continue;
		(*found)++;
		if (!phdr.p_align ||
		    phdr.p_offset % phdr.p_align != phdr.p_vaddr % phdr.p_align)
			(*misaligned)++;
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
	unsigned int found = 0;
	unsigned int misaligned = 0;
	size_t old_pattern_length = 0;
	bool inspected = false;
	int status = 0;
	pid_t pid = -1;
	pid_t waited;

	ksft_print_header();
	ksft_set_plan(5);
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
	ksft_test_result(true, "install a temporary core pattern\n");

	pid = fork();
	if (!pid)
		crash_with_test_vmas();
	if (pid < 0) {
		ksft_test_result_fail("generate a core from the 4K process\n");
		ksft_test_result_fail("parse the generated ELF core\n");
		ksft_test_result_fail("keep 4K VMA offsets aligned in PT_LOAD headers\n");
		ksft_print_msg("fork failed: %s\n", strerror(errno));
		goto restore_pattern;
	}
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != pid) {
		ksft_test_result_fail("generate a core from the 4K process\n");
		ksft_test_result_fail("parse the generated ELF core\n");
		ksft_test_result_fail("keep 4K VMA offsets aligned in PT_LOAD headers\n");
		ksft_print_msg("waitpid failed: %s\n", strerror(errno));
		goto remove_core;
	}
	ksft_test_result(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
			 "generate a core from the 4K process\n");

	snprintf(core_path, sizeof(core_path), "/tmp/elf-core-ppps-%d", pid);
	inspected = inspect_core(core_path, &found, &misaligned);
	ksft_test_result(inspected, "parse the generated ELF core\n");
	ksft_test_result(inspected && found == TEST_VMAS && !misaligned,
			 "keep 4K VMA offsets aligned in PT_LOAD headers\n");
	ksft_print_msg("test VMAs=%u misaligned PT_LOADs=%u\n",
		       found, misaligned);

remove_core:
	snprintf(core_path, sizeof(core_path), "/tmp/elf-core-ppps-%d", pid);
	unlink(core_path);

restore_pattern:
	if (!write_file("/proc/sys/kernel/core_pattern", old_pattern,
			old_pattern_length))
		ksft_print_msg("warning: could not restore core_pattern\n");
	free(old_pattern);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
