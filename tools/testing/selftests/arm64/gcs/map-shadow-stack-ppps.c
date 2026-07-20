// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for 4K processes using arm64 GCS on PPPS kernels.
 */

#define _GNU_SOURCE
#include <asm/hwcap.h>
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
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __NR_map_shadow_stack
#define __NR_map_shadow_stack 453
#endif

#ifndef PR_SET_SHADOW_STACK_STATUS
#define PR_SET_SHADOW_STACK_STATUS 75
#endif

#ifndef PR_SHADOW_STACK_ENABLE
#define PR_SHADOW_STACK_ENABLE 1UL
#endif

#define PROCESS_PAGE_SIZE 4096UL
#define NATIVE_16K_SIZE 16384UL

static int test_no;
static int failures;

static __always_inline long
raw_syscall5(long number, long arg1, long arg2, long arg3, long arg4, long arg5)
{
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;
	register long x2 __asm__("x2") = arg3;
	register long x3 __asm__("x3") = arg4;
	register long x4 __asm__("x4") = arg5;
	register long x8 __asm__("x8") = number;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
			 : "memory", "cc");
	return x0;
}

static void result(bool pass, const char *name)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++test_no, name);
	if (!pass)
		failures++;
}

static ssize_t shadow_stack_vma_size(pid_t pid, uintptr_t address)
{
	char path[64];
	char *line = NULL;
	size_t capacity = 0;
	unsigned long start = 0, end = 0;
	ssize_t size = -1;
	FILE *file;

	if (pid)
		snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	else
		strcpy(path, "/proc/self/smaps");

	file = fopen(path, "re");
	if (!file)
		return -1;

	while (getline(&line, &capacity, file) >= 0) {
		unsigned long next_start, next_end;

		if (sscanf(line, "%lx-%lx", &next_start, &next_end) == 2) {
			start = next_start;
			end = next_end;
			continue;
		}

		if (strncmp(line, "VmFlags:", 8) || !strstr(line, " ss"))
			continue;
		if (!address || (address >= start && address < end)) {
			size = end - start;
			break;
		}
	}

	free(line);
	fclose(file);
	return size;
}

static bool test_fixed_process_page(void)
{
	void *reservation, *mapped;
	uintptr_t candidate;
	ssize_t size;

	reservation = mmap(NULL, 8 * NATIVE_16K_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;

	candidate = ((uintptr_t)reservation + NATIVE_16K_SIZE - 1) &
		    ~(NATIVE_16K_SIZE - 1);
	candidate += PROCESS_PAGE_SIZE;
	if (munmap((void *)candidate, PROCESS_PAGE_SIZE)) {
		munmap(reservation, 8 * NATIVE_16K_SIZE);
		return false;
	}

	errno = 0;
	mapped = (void *)syscall(__NR_map_shadow_stack, candidate,
				 PROCESS_PAGE_SIZE, 0);
	if (mapped == MAP_FAILED) {
		printf("# fixed 4K-aligned map failed: errno=%d\n", errno);
		munmap(reservation, 8 * NATIVE_16K_SIZE);
		return false;
	}

	size = shadow_stack_vma_size(0, candidate);
	munmap(mapped, PROCESS_PAGE_SIZE);
	munmap(reservation, 8 * NATIVE_16K_SIZE);
	if (mapped != (void *)candidate)
		return false;
	if (size != PROCESS_PAGE_SIZE)
		printf("# fixed mapping VMA size=%zd, expected=%lu\n",
		       size, PROCESS_PAGE_SIZE);
	return size == PROCESS_PAGE_SIZE;
}

static bool test_kernel_selected_process_page(void)
{
	void *mapped;
	ssize_t size;

	errno = 0;
	mapped = (void *)syscall(__NR_map_shadow_stack, 0,
				 PROCESS_PAGE_SIZE, 0);
	if (mapped == MAP_FAILED) {
		printf("# kernel-selected map failed: errno=%d\n", errno);
		return false;
	}

	size = shadow_stack_vma_size(0, (uintptr_t)mapped);
	munmap(mapped, size > 0 ? (size_t)size : PROCESS_PAGE_SIZE);
	if (size != PROCESS_PAGE_SIZE)
		printf("# kernel-selected VMA size=%zd, expected=%lu\n",
		       size, PROCESS_PAGE_SIZE);
	return size == PROCESS_PAGE_SIZE;
}

static bool test_process_page_guard(void)
{
	void *reservation, *shadow_mapping = MAP_FAILED;
	void *neighbor = MAP_FAILED;
	uintptr_t candidate, shadow;
	bool success = false;

	reservation = mmap(NULL, 8 * NATIVE_16K_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;

	shadow = ((uintptr_t)reservation + NATIVE_16K_SIZE - 1) &
		 ~(NATIVE_16K_SIZE - 1);
	shadow += 2 * NATIVE_16K_SIZE;
	candidate = shadow - 2 * PROCESS_PAGE_SIZE;
	if (munmap((void *)candidate, 3 * PROCESS_PAGE_SIZE))
		goto out;

	shadow_mapping = (void *)syscall(__NR_map_shadow_stack, shadow,
					 PROCESS_PAGE_SIZE, 0);
	if (shadow_mapping != (void *)shadow)
		goto out;

	neighbor = mmap((void *)candidate, PROCESS_PAGE_SIZE, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	success = neighbor == (void *)candidate;
out:
	if (neighbor != MAP_FAILED)
		munmap(neighbor, PROCESS_PAGE_SIZE);
	if (shadow_mapping != MAP_FAILED)
		munmap(shadow_mapping, PROCESS_PAGE_SIZE);
	munmap(reservation, 8 * NATIVE_16K_SIZE);
	return success;
}

static bool test_default_process_page_stack(void)
{
	struct child_status {
		int ready;
		int error;
	} status = { 0 };
	struct rlimit limit = {
		.rlim_cur = 2 * PROCESS_PAGE_SIZE,
		.rlim_max = 2 * PROCESS_PAGE_SIZE,
	};
	int pipefd[2];
	ssize_t size;
	pid_t child;

	if (pipe(pipefd))
		return false;
	child = fork();
	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}
	if (!child) {
		long ret;

		close(pipefd[0]);
		if (setrlimit(RLIMIT_STACK, &limit)) {
			status.error = errno;
		} else {
			ret = raw_syscall5(__NR_prctl, PR_SET_SHADOW_STACK_STATUS,
					   PR_SHADOW_STACK_ENABLE, 0, 0, 0);
			if (ret < 0)
				status.error = -ret;
			else
				status.ready = 1;
		}
		raw_syscall5(__NR_write, pipefd[1], (long)&status,
			     sizeof(status), 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}

	close(pipefd[1]);
	if (read(pipefd[0], &status, sizeof(status)) != sizeof(status))
		status.ready = 0;
	close(pipefd[0]);
	size = status.ready ? shadow_stack_vma_size(child, 0) : -1;
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	if (!status.ready)
		printf("# child failed to enable GCS: errno=%d\n", status.error);
	else if (size != PROCESS_PAGE_SIZE)
		printf("# default GCS VMA size=%zd, expected=%lu\n",
		       size, PROCESS_PAGE_SIZE);
	return status.ready && size == PROCESS_PAGE_SIZE;
}

int main(void)
{
	printf("TAP version 13\n");
	printf("1..5\n");

	result(getpagesize() == PROCESS_PAGE_SIZE,
	       "process reports a 4K page size");
	if (!(getauxval(AT_HWCAP) & HWCAP_GCS)) {
		printf("ok 2 - FEAT_GCS is available # SKIP\n");
		printf("ok 3 - FEAT_GCS is available # SKIP\n");
		printf("ok 4 - FEAT_GCS is available # SKIP\n");
		printf("ok 5 - FEAT_GCS is available # SKIP\n");
		return failures ? EXIT_FAILURE : 4;
	}

	result(test_fixed_process_page(),
	       "map a GCS at a 4K-aligned, non-16K-aligned address");
	result(test_kernel_selected_process_page(),
	       "round a requested GCS mapping to one process page");
	result(test_process_page_guard(),
	       "leave one process-page guard below a GCS mapping");
	result(test_default_process_page_stack(),
	       "size the minimum default GCS stack in process pages");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
