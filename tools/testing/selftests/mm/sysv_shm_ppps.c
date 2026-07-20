// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/shm.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define RESERVE_SIZE	(4 * NATIVE_PAGE_SIZE)
#define TEST_VALUE	0x5a17c0deU

struct target_area {
	void *base;
	void *target;
};

static bool reserve_subpage_target(struct target_area *area)
{
	uintptr_t start;
	uintptr_t target;

	area->base = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area->base == MAP_FAILED)
		return false;
	start = (uintptr_t)area->base;
	target = (start + NATIVE_PAGE_SIZE - 1) & -NATIVE_PAGE_SIZE;
	target += USER_PAGE_SIZE;
	if (target + USER_PAGE_SIZE > start + RESERVE_SIZE) {
		munmap(area->base, RESERVE_SIZE);
		area->base = MAP_FAILED;
		return false;
	}
	area->target = (void *)target;
	return true;
}

static void release_target(struct target_area *area)
{
	if (area->base != MAP_FAILED)
		munmap(area->base, RESERVE_SIZE);
}

static int create_segment(void)
{
	return shmget(IPC_PRIVATE, USER_PAGE_SIZE, IPC_CREAT | 0600);
}

static bool fixed_attach_roundtrip(bool *detach_ok)
{
	struct target_area area = { .base = MAP_FAILED };
	unsigned int *mapping = (void *)-1;
	bool success = false;
	int shmid;

	*detach_ok = false;
	shmid = create_segment();
	if (shmid < 0 || !reserve_subpage_target(&area))
		goto out;
	mapping = shmat(shmid, area.target, SHM_REMAP);
	if (mapping == (void *)-1)
		goto out;
	*mapping = TEST_VALUE;
	success = *mapping == TEST_VALUE;
	*detach_ok = shmdt(mapping) == 0;
	mapping = (void *)-1;
out:
	if (mapping != (void *)-1)
		shmdt(mapping);
	if (shmid >= 0)
		shmctl(shmid, IPC_RMID, NULL);
	release_target(&area);
	return success;
}

static bool rounded_attach_roundtrip(void)
{
	struct target_area area = { .base = MAP_FAILED };
	unsigned int *mapping = (void *)-1;
	bool success = false;
	int shmid;

	shmid = create_segment();
	if (shmid < 0 || !reserve_subpage_target(&area))
		goto out;
	mapping = shmat(shmid, area.target, SHM_RND | SHM_REMAP);
	if (mapping == (void *)-1)
		goto out;
	*mapping = TEST_VALUE;
	success = mapping == area.target && *mapping == TEST_VALUE;
out:
	if (mapping != (void *)-1 && shmdt(mapping))
		success = false;
	if (shmid >= 0)
		shmctl(shmid, IPC_RMID, NULL);
	release_target(&area);
	return success;
}

static bool moved_attach_roundtrip(bool *detach_ok)
{
	struct target_area area = { .base = MAP_FAILED };
	unsigned int *mapping = (void *)-1;
	void *moved = MAP_FAILED;
	bool success = false;
	int shmid;

	*detach_ok = false;
	shmid = create_segment();
	if (shmid < 0)
		goto out;
	mapping = shmat(shmid, NULL, 0);
	if (mapping == (void *)-1 || !reserve_subpage_target(&area))
		goto out;
	*mapping = TEST_VALUE;
	moved = mremap(mapping, USER_PAGE_SIZE, USER_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, area.target);
	if (moved == MAP_FAILED)
		goto out;
	mapping = (void *)-1;
	success = *(unsigned int *)moved == TEST_VALUE;
	*detach_ok = shmdt(moved) == 0;
	moved = MAP_FAILED;
out:
	if (moved != MAP_FAILED)
		munmap(moved, USER_PAGE_SIZE);
	if (mapping != (void *)-1)
		shmdt(mapping);
	if (shmid >= 0)
		shmctl(shmid, IPC_RMID, NULL);
	release_target(&area);
	return success;
}

static int run_test(void)
{
	bool fixed_detach;
	bool moved_detach;
	bool fixed;
	bool moved;
	bool rounded;
	int probe;

	ksft_print_header();
	if (access("/proc/sysvipc/shm", F_OK))
		ksft_exit_skip("CONFIG_SYSVIPC is disabled\n");
	probe = create_segment();
	if (probe < 0)
		ksft_exit_fail_msg("SysV shm probe failed: %s\n",
				   strerror(errno));
	shmctl(probe, IPC_RMID, NULL);
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fixed = fixed_attach_roundtrip(&fixed_detach);
	ksft_test_result(fixed,
			 "attach SysV shm at a 4K-aligned subpage address\n");
	ksft_test_result(fixed && fixed_detach,
			 "detach the fixed subpage SysV shm mapping\n");
	rounded = rounded_attach_roundtrip();
	ksft_test_result(rounded,
			 "round SysV shm addresses at process-page granularity\n");
	moved = moved_attach_roundtrip(&moved_detach);
	ksft_test_result(moved,
			 "mremap SysV shm to a 4K-aligned subpage address\n");
	ksft_test_result(moved && moved_detach,
			 "detach the moved subpage SysV shm mapping\n");
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
	execl("/proc/self/exe", "sysv_shm_ppps", "--run", NULL);
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
