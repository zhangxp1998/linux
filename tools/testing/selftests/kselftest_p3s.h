/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared helpers for the P3S (per-process page size) selftests.
 *
 * A P3S kernel is a native 16K arm64 kernel on which a process carrying the
 * ADDR_PAGE_SIZE_4KB personality bit sees 4K pages.  Nearly every test
 * in this corpus starts as whatever the runner is, re-execs itself as a 4K
 * process and only then runs its checks; a few also spawn a native (16K)
 * helper. This header holds the common process-page helpers.
 *
 * The 4K process interface is deliberately minimal: whatever a test observes
 * that is not part of that interface is expected to behave as it does for a
 * native 16K process.
 */
#ifndef __KSELFTEST_P3S_H
#define __KSELFTEST_P3S_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "kselftest.h"

/*
 * personality(2) bit selecting the 4KB process page size at the next exec.
 * The uapi <linux/personality.h> carries it as an enumerator, which the libc
 * <sys/personality.h> does not re-export, hence the macro.
 */
#define ADDR_PAGE_SIZE_4KB	0x40000000

/* Page size of a 4KB process and of the native kernel. */
#define PROCESS_PAGE_SIZE		4096UL
#define NATIVE_PAGE_SIZE		16384UL

/* Number of 4K process pages ("slices") backed by one native page. */
#define P3S_SLICES			(NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE)

/* Reuse the running binary's name when forked helpers re-exec a role. */
static const char *p3s_argv0 __maybe_unused = "/proc/self/exe";

#define P3S_MAX_EXEC_ARGS 15

/*
 * A native 4K kernel can retain ADDR_PAGE_SIZE_4KB after exec too.  The
 * personality bit and AT_PAGESZ alone therefore do not prove that the
 * process has 4K PTEs backed by native 16K pages. KernelPageSize and
 * MMUPageSize in smaps report the geometry independently of AT_PAGESZ.
 *
 * MMUPageSize of the first VMA if the kernel uses 16K pages, else 0.
 */
static inline unsigned long p3s_mmu_page_size(void)
{
	unsigned long kernel_kb = 0, mmu_kb = 0, kilobytes;
	char line[256];
	FILE *smaps = fopen("/proc/self/smaps", "re");

	if (!smaps)
		return 0;
	while (fgets(line, sizeof(line), smaps)) {
		if (sscanf(line, "KernelPageSize: %lu kB", &kilobytes) == 1)
			kernel_kb = kilobytes;
		else if (sscanf(line, "MMUPageSize: %lu kB", &kilobytes) == 1)
			mmu_kb = kilobytes;
		if (kernel_kb && mmu_kb) {
			fclose(smaps);
			return kernel_kb * 1024 == NATIVE_PAGE_SIZE ?
			       mmu_kb * 1024 : 0;
		}
	}
	fclose(smaps);
	return 0;
}

static inline bool p3s_has_native_pages(void)
{
	return p3s_mmu_page_size() == NATIVE_PAGE_SIZE;
}

static inline bool p3s_is_4kb_process(void)
{
	int persona = personality(0xffffffffUL);

	return persona >= 0 && (persona & ADDR_PAGE_SIZE_4KB) &&
	       p3s_mmu_page_size() == PROCESS_PAGE_SIZE;
}

/* Hard precondition of every 4KB-side test body. */
static inline void p3s_require_4kb(void)
{
	if (!p3s_is_4kb_process())
		ksft_exit_skip("P3S 4KB process unavailable\n");
}

/* Re-exec /proc/self/exe with the 4KB bit set or cleared.  -1 with errno. */
static inline int p3s_execv(bool four_kb, char *const argv[])
{
	int orig_persona = personality(0xffffffffUL);
	int target_persona;
	int saved_errno;

	if (orig_persona < 0)
		return -1;
	target_persona = four_kb ? orig_persona | ADDR_PAGE_SIZE_4KB :
				   orig_persona & ~ADDR_PAGE_SIZE_4KB;
	if (personality(target_persona) < 0)
		return -1;
	execv("/proc/self/exe", argv);
	saved_errno = errno;
	personality(orig_persona);
	errno = saved_errno;
	return -1;
}

/* Collect the NULL-terminated list after @argv0 into @argv. */
static inline bool p3s_build_argv(const char *argv[P3S_MAX_EXEC_ARGS + 1],
				  const char *argv0, va_list ap)
{
	unsigned int i = 1;
	const char *arg;

	argv[0] = argv0 ? argv0 : p3s_argv0;
	while ((arg = va_arg(ap, const char *)) != NULL) {
		if (i >= P3S_MAX_EXEC_ARGS)
			return false;
		argv[i++] = arg;
	}
	argv[i] = NULL;
	return true;
}

/*
 * execl()-style re-exec that returns -1 (with errno) on failure, for forked
 * helpers that must fail quietly rather than bail out of the TAP stream.
 * @argv0 may be NULL to reuse the recorded argv[0].
 */
static inline int __attribute__((sentinel))
p3s_execl(bool four_kb, const char *argv0, ...)
{
	const char *argv[P3S_MAX_EXEC_ARGS + 1];
	va_list ap;
	bool ok;

	va_start(ap, argv0);
	ok = p3s_build_argv(argv, argv0, ap);
	va_end(ap);
	if (!ok) {
		errno = E2BIG;
		return -1;
	}
	return p3s_execv(four_kb, (char *const *)argv);
}

/* Run a role of the same binary in either geometry and return its status. */
static inline int __attribute__((sentinel))
p3s_run_child(bool four_kb, const char *role, ...)
{
	const char *argv[P3S_MAX_EXEC_ARGS + 1];
	const char *arg;
	va_list ap;
	pid_t child;
	int status;
	int i = 2;

	if (!role)
		return KSFT_FAIL;
	argv[0] = p3s_argv0;
	argv[1] = role;
	va_start(ap, role);
	while ((arg = va_arg(ap, const char *)) != NULL) {
		if (i >= P3S_MAX_EXEC_ARGS) {
			va_end(ap);
			return KSFT_FAIL;
		}
		argv[i++] = arg;
	}
	va_end(ap);
	argv[i] = NULL;
	child = fork();
	if (child < 0)
		return KSFT_FAIL;
	if (!child) {
		p3s_execv(four_kb, (char *const *)argv);
		_exit(errno == EINVAL || errno == EPERM ? KSFT_SKIP : KSFT_FAIL);
	}
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return KSFT_FAIL;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : KSFT_FAIL;
}

/* Whole-buffer I/O, retrying short transfers and EINTR. */

static inline bool read_full(int fd, void *buffer, size_t size)
{
	unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = read(fd, position, size);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		size -= bytes;
	}
	return true;
}

static inline bool write_full(int fd, const void *buffer, size_t size)
{
	const unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = write(fd, position, size);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		size -= bytes;
	}
	return true;
}

static inline bool pwrite_full(int fd, const void *buffer, size_t size,
			       off_t offset)
{
	const unsigned char *position = buffer;

	while (size) {
		ssize_t bytes = pwrite(fd, position, size, offset);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return false;
		position += bytes;
		offset += bytes;
		size -= bytes;
	}
	return true;
}

/* Test fixtures (device nodes, debugfs files, ...) */

/* An error from opening a fixture that means "not on this system". */
static inline bool p3s_fixture_unavailable(int error)
{
	return error == ENOENT || error == ENXIO || error == ENODEV ||
	       error == ENOTDIR || error == EACCES || error == EPERM;
}

/* open(2) a fixture with O_CLOEXEC; -1 with errno preserved on failure. */
static inline int p3s_open_fixture(const char *path, int flags)
{
	return open(path, flags | O_CLOEXEC);
}

#endif /* __KSELFTEST_P3S_H */
