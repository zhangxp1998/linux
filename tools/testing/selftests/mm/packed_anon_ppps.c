// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define PROCESS_PAGE	4096UL
#define NATIVE_PAGE	16384UL
#define SLICES		(NATIVE_PAGE / PROCESS_PAGE)
#define PAGEMAP_PRESENT	UINT64_C(0x8000000000000000)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define DEPACK_RACE_ATTEMPTS 1000

static unsigned char *map_aligned(size_t size, unsigned char **reservation)
{
	unsigned char *mapping;
	uintptr_t aligned;

	mapping = mmap(NULL, size + NATIVE_PAGE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)mapping + NATIVE_PAGE - 1) &
		  ~(NATIVE_PAGE - 1);
	*reservation = mapping;
	return (unsigned char *)aligned;
}

static void populate(unsigned char *base)
{
	unsigned int i;

	for (i = 0; i < SLICES; i++)
		memset(base + i * PROCESS_PAGE, 0x31 + i, PROCESS_PAGE);
}

static bool verify(const unsigned char *base)
{
	unsigned long offset;

	for (offset = 0; offset < NATIVE_PAGE; offset++)
		if (base[offset] != 0x31 + offset / PROCESS_PAGE)
			return false;
	return true;
}

static bool range_stat_bytes(const void *address, unsigned long length,
			     const char *name, unsigned long *bytes)
{
	unsigned long target_start = (unsigned long)address;
	unsigned long target_end = target_start + length;
	unsigned long start, end, value_kb;
	char format[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	FILE *smaps;

	*bytes = 0;
	snprintf(format, sizeof(format), "%s: %%lu kB", name);
	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = target_start < end && target_end > start;
			continue;
		}
		if (in_target && sscanf(line, format, &value_kb) == 1) {
			*bytes += value_kb * 1024;
			found = true;
			in_target = false;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

static bool packed_smaps_matches(const unsigned char *base,
				 unsigned long pss,
				 unsigned long shared_dirty,
				 unsigned long private_dirty)
{
	unsigned long value;

	return range_stat_bytes(base, NATIVE_PAGE, "Rss", &value) &&
		value == NATIVE_PAGE &&
		range_stat_bytes(base, NATIVE_PAGE, "Pss", &value) &&
		value == pss &&
		range_stat_bytes(base, NATIVE_PAGE, "Pss_Dirty", &value) &&
		value == pss &&
		range_stat_bytes(base, NATIVE_PAGE, "Shared_Dirty", &value) &&
		value == shared_dirty &&
		range_stat_bytes(base, NATIVE_PAGE, "Private_Dirty", &value) &&
		value == private_dirty;
}

static bool fork_smaps_shares_tuple(unsigned char *base)
{
	int child_ready[2];
	int child_done[2];
	char byte;
	pid_t child;
	int status;
	bool passed;

	if (pipe(child_ready))
		return false;
	if (pipe(child_done)) {
		close(child_ready[0]);
		close(child_ready[1]);
		return false;
	}
	child = fork();
	if (child < 0) {
		close(child_ready[0]);
		close(child_ready[1]);
		close(child_done[0]);
		close(child_done[1]);
		return false;
	}
	if (!child) {
		close(child_ready[0]);
		close(child_done[1]);
		if (write(child_ready[1], "R", 1) != 1 ||
		    read(child_done[0], &byte, 1) != 1)
			_exit(1);
		_exit(verify(base) ? 0 : 1);
	}

	close(child_ready[1]);
	close(child_done[0]);
	passed = read(child_ready[0], &byte, 1) == 1 &&
		 packed_smaps_matches(base, NATIVE_PAGE / 2, NATIVE_PAGE, 0);
	if (write(child_done[1], "D", 1) != 1)
		passed = false;
	close(child_ready[0]);
	close(child_done[1]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		passed = false;
	return passed;
}

static bool read_pagemap_pfns(const unsigned char *const address[SLICES],
			      uint64_t pfn[SLICES])
{
	off_t offset;
	uint64_t entry;
	int fd;
	unsigned int i;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	for (i = 0; i < SLICES; i++) {
		offset = ((uintptr_t)address[i] / PROCESS_PAGE) *
			 sizeof(entry);
		if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
		    !(entry & PAGEMAP_PRESENT)) {
			close(fd);
			return false;
		}
		pfn[i] = entry & PAGEMAP_PFN_MASK;
	}
	close(fd);
	return true;
}

static bool read_pfns(const unsigned char *base, uint64_t pfn[SLICES])
{
	const unsigned char *address[SLICES];
	unsigned int i;

	for (i = 0; i < SLICES; i++)
		address[i] = base + i * PROCESS_PAGE;
	return read_pagemap_pfns(address, pfn);
}

static bool read_moved_pfns(const unsigned char *dst,
			    const unsigned char *src, uint64_t pfn[SLICES])
{
	const unsigned char *address[SLICES] = {
		dst,
		src + PROCESS_PAGE,
		src + 2 * PROCESS_PAGE,
		src + 3 * PROCESS_PAGE,
	};

	return read_pagemap_pfns(address, pfn);
}

static bool same_pfn(const uint64_t pfn[SLICES])
{
	unsigned int i;

	if (!pfn[0])
		return false;
	for (i = 1; i < SLICES; i++)
		if (pfn[i] != pfn[0])
			return false;
	return true;
}

static bool distinct_pfns(const uint64_t pfn[SLICES])
{
	unsigned int i, j;

	for (i = 0; i < SLICES; i++) {
		if (!pfn[i])
			return false;
		for (j = i + 1; j < SLICES; j++)
			if (pfn[i] == pfn[j])
				return false;
	}
	return true;
}

static bool verify_process_vm_pattern(const unsigned char *base,
				      unsigned char first)
{
	unsigned long offset;

	for (offset = 0; offset < NATIVE_PAGE; offset++)
		if (base[offset] != first + offset / PROCESS_PAGE)
			return false;
	return true;
}

static void process_vm_rw_preserves_packed_slices(unsigned char *base,
						  bool *read_ok, bool *write_ok)
{
	unsigned char local[NATIVE_PAGE];
	struct iovec local_iov = {
		.iov_base = local,
		.iov_len = sizeof(local),
	};
	struct iovec remote_iov = {
		.iov_base = base,
		.iov_len = NATIVE_PAGE,
	};
	uint64_t pfn[SLICES];
	unsigned int i;
	bool packed;

	populate(base);
	packed = read_pfns(base, pfn) && same_pfn(pfn);
	memset(local, 0, sizeof(local));
	*read_ok = packed &&
		process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0) ==
		NATIVE_PAGE && verify(local);

	for (i = 0; i < SLICES; i++)
		memset(local + i * PROCESS_PAGE, 0x61 + i, PROCESS_PAGE);
	*write_ok = packed &&
		process_vm_writev(getpid(), &local_iov, 1, &remote_iov, 1, 0) ==
		NATIVE_PAGE && verify_process_vm_pattern(base, 0x61);
}

static bool mremap_handles_packed_tuple(bool same_slice)
{
	unsigned char *src_reservation = NULL;
	unsigned char *dst_reservation = NULL;
	unsigned char *dst_base;
	unsigned char *src;
	unsigned char *dst;
	uint64_t pfn[SLICES];
	void *moved;
	bool passed = false;

	src = map_aligned(2 * NATIVE_PAGE, &src_reservation);
	dst_base = map_aligned(3 * NATIVE_PAGE, &dst_reservation);
	if (src == MAP_FAILED || dst_base == MAP_FAILED)
		goto out;
	dst = dst_base + (same_slice ? 0 : PROCESS_PAGE);
	populate(src);
	if (!read_pfns(src, pfn) || !same_pfn(pfn))
		goto out;

	moved = mremap(src, NATIVE_PAGE, NATIVE_PAGE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	passed = moved == dst && verify(dst) && read_pfns(dst, pfn) &&
		 (same_slice ? same_pfn(pfn) : distinct_pfns(pfn));
out:
	if (src_reservation)
		munmap(src_reservation, 3 * NATIVE_PAGE);
	if (dst_reservation)
		munmap(dst_reservation, 4 * NATIVE_PAGE);
	return passed;
}

static bool fork_cow_preserves_parent(unsigned char *base)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		base[PROCESS_PAGE] = 0xa5;
		if (base[PROCESS_PAGE] != 0xa5 ||
		    base[0] != 0x31 || base[2 * PROCESS_PAGE] != 0x33 ||
		    base[3 * PROCESS_PAGE] != 0x34)
			_exit(1);
		_exit(0);
	}
	if (waitpid(child, &status, 0) != child)
		return false;
	return WIFEXITED(status) && !WEXITSTATUS(status) && verify(base);
}

struct depack_race_args {
	unsigned char *base;
	atomic_bool ready;
	atomic_bool stop;
};

static void *write_during_depack(void *data)
{
	struct depack_race_args *args = data;
	atomic_uchar *target = (atomic_uchar *)args->base;
	unsigned char value = 0;

	atomic_store_explicit(&args->ready, true, memory_order_release);
	while (!atomic_load_explicit(&args->stop, memory_order_acquire))
		atomic_store_explicit(target, ++value, memory_order_relaxed);
	return NULL;
}

static bool concurrent_partial_madvise(void)
{
	unsigned int attempt;

	for (attempt = 0; attempt < DEPACK_RACE_ATTEMPTS; attempt++) {
		struct depack_race_args args;
		unsigned char *reservation;
		unsigned char *base;
		pthread_t thread;
		bool passed;
		int error;
		int ret;

		base = map_aligned(2 * NATIVE_PAGE, &reservation);
		if (base == MAP_FAILED)
			return false;
		populate(base);
		args.base = base;
		atomic_init(&args.ready, false);
		atomic_init(&args.stop, false);
		ret = pthread_create(&thread, NULL, write_during_depack, &args);
		if (ret) {
			munmap(reservation, 3 * NATIVE_PAGE);
			return false;
		}
		while (!atomic_load_explicit(&args.ready, memory_order_acquire))
			sched_yield();
		for (ret = 0; ret < 64; ret++)
			sched_yield();

		errno = 0;
		ret = madvise(base + PROCESS_PAGE, PROCESS_PAGE,
			      MADV_DONTNEED);
		error = errno;
		atomic_store_explicit(&args.stop, true, memory_order_release);
		passed = !pthread_join(thread, NULL) && !ret &&
			 base[PROCESS_PAGE] == 0 &&
			 base[2 * PROCESS_PAGE] == 0x33 &&
			 base[3 * PROCESS_PAGE] == 0x34;
		munmap(reservation, 3 * NATIVE_PAGE);
		if (!passed) {
			errno = error;
			return false;
		}
	}
	return true;
}

struct uffd_move_result {
	bool supported;
	bool register_depacked;
	bool move_preserved;
	bool unregistered_source_preserved;
};

static struct uffd_move_result userfaultfd_move_depacks(void)
{
	struct uffd_move_result result = {};
	struct uffdio_register uffd_register = {};
	struct uffdio_range uffd_unregister = {};
	struct uffdio_move uffd_move = {};
	struct uffdio_api uffd_api = {};
	unsigned char *reservation;
	unsigned char *src_reservation = NULL;
	unsigned char *dst_reservation = NULL;
	unsigned char *register_base;
	unsigned char *base;
	unsigned char *dst;
	unsigned char *src_unregistered = MAP_FAILED;
	unsigned char *dst_registered = MAP_FAILED;
	uint64_t pfn[SLICES];
	bool initial_packed;
	int uffd = -1;

	register_base = map_aligned(5 * NATIVE_PAGE, &reservation);
	if (register_base == MAP_FAILED)
		return result;
	base = register_base + NATIVE_PAGE;
	dst = register_base + 3 * NATIVE_PAGE;
	populate(base);
	initial_packed = read_pfns(base, pfn) && same_pfn(pfn);

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (uffd < 0)
		goto out;
	uffd_api.api = UFFD_API;
	if (ioctl(uffd, UFFDIO_API, &uffd_api) ||
	    !(uffd_api.features & UFFD_FEATURE_MOVE))
		goto out;
	result.supported = true;

	uffd_register.range.start = (uintptr_t)register_base;
	uffd_register.range.len = 5 * NATIVE_PAGE;
	uffd_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffd_register))
		goto out;

	result.register_depacked = initial_packed && read_pfns(base, pfn) &&
				     distinct_pfns(pfn) && verify(base);

	uffd_move.dst = (uintptr_t)dst;
	uffd_move.src = (uintptr_t)base;
	uffd_move.len = PROCESS_PAGE;
	if (!ioctl(uffd, UFFDIO_MOVE, &uffd_move) &&
	    uffd_move.move == PROCESS_PAGE && dst[0] == 0x31 &&
	    base[PROCESS_PAGE] == 0x32 &&
	    base[2 * PROCESS_PAGE] == 0x33 &&
	    base[3 * PROCESS_PAGE] == 0x34 &&
	    read_moved_pfns(dst, base, pfn) && distinct_pfns(pfn))
		result.move_preserved = true;

	uffd_unregister.start = (uintptr_t)register_base;
	uffd_unregister.len = 5 * NATIVE_PAGE;
	if (ioctl(uffd, UFFDIO_UNREGISTER, &uffd_unregister))
		result.move_preserved = false;
	else if (base[0] || base[PROCESS_PAGE] != 0x32 ||
		 base[2 * PROCESS_PAGE] != 0x33 ||
		 base[3 * PROCESS_PAGE] != 0x34)
		result.move_preserved = false;

	src_unregistered = map_aligned(2 * NATIVE_PAGE, &src_reservation);
	dst_registered = map_aligned(2 * NATIVE_PAGE, &dst_reservation);
	if (src_unregistered == MAP_FAILED || dst_registered == MAP_FAILED)
		goto out;
	populate(src_unregistered);
	if (!read_pfns(src_unregistered, pfn) || !same_pfn(pfn))
		goto out;

	memset(&uffd_register, 0, sizeof(uffd_register));
	uffd_register.range.start = (uintptr_t)dst_registered;
	uffd_register.range.len = NATIVE_PAGE;
	uffd_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffd_register))
		goto out;

	memset(&uffd_move, 0, sizeof(uffd_move));
	uffd_move.dst = (uintptr_t)dst_registered;
	uffd_move.src = (uintptr_t)src_unregistered;
	uffd_move.len = PROCESS_PAGE;
	if (!ioctl(uffd, UFFDIO_MOVE, &uffd_move) &&
	    uffd_move.move == PROCESS_PAGE && dst_registered[0] == 0x31 &&
	    src_unregistered[PROCESS_PAGE] == 0x32 &&
	    src_unregistered[2 * PROCESS_PAGE] == 0x33 &&
	    src_unregistered[3 * PROCESS_PAGE] == 0x34 &&
	    read_moved_pfns(dst_registered, src_unregistered, pfn) &&
	    distinct_pfns(pfn))
		result.unregistered_source_preserved = true;

	uffd_unregister.start = (uintptr_t)dst_registered;
	uffd_unregister.len = NATIVE_PAGE;
	if (ioctl(uffd, UFFDIO_UNREGISTER, &uffd_unregister) ||
	    src_unregistered[0])
		result.unregistered_source_preserved = false;

out:
	if (uffd >= 0)
		close(uffd);
	if (src_reservation && src_unregistered != MAP_FAILED)
		munmap(src_reservation, 3 * NATIVE_PAGE);
	if (dst_reservation && dst_registered != MAP_FAILED)
		munmap(dst_reservation, 3 * NATIVE_PAGE);
	munmap(reservation, 6 * NATIVE_PAGE);
	return result;
}

static int run_test(void)
{
	struct uffd_move_result uffd_move;
	unsigned char *reservation;
	unsigned char *base;
	uint64_t pfn[SLICES];
	unsigned long rss_before = 0;
	unsigned long rss_after = 0;
	bool have_pfns;
	bool passed;
	bool process_vm_read_ok;
	bool process_vm_write_ok;

	ksft_print_header();
	ksft_set_plan(19);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE,
			 "process uses 4K pages\n");

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	ksft_test_result(base != MAP_FAILED, "map anonymous test range\n");
	if (base == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	populate(base);
	if (!verify(base))
		ksft_exit_fail_msg("writable packed slices corrupted\n");
	if (madvise(base, NATIVE_PAGE, MADV_DONTNEED) ||
	    !range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss_before))
		rss_before = ~0UL;
	populate(base);
	if (!range_stat_bytes(base, NATIVE_PAGE, "Rss", &rss_after))
		rss_after = 0;
	have_pfns = read_pfns(base, pfn);
	ksft_test_result(have_pfns && same_pfn(pfn),
			 "four adjacent PTEs map one native folio\n");
	ksft_test_result(rss_before != ~0UL && rss_after >= rss_before &&
			 rss_after - rss_before == NATIVE_PAGE,
			 "packed tuple accounts one native RSS page (%lu bytes)\n",
			 rss_after - rss_before);
	ksft_test_result(packed_smaps_matches(base, NATIVE_PAGE, 0,
					      NATIVE_PAGE),
			 "smaps counts one private packed tuple at full PSS\n");
	ksft_test_result(fork_smaps_shares_tuple(base),
			 "smaps shares packed tuple PSS once across fork\n");

	ksft_test_result(fork_cow_preserves_parent(base),
			 "fork COW preserves all four slices\n");

	passed = !mprotect(base + PROCESS_PAGE, PROCESS_PAGE, PROT_READ) &&
		 read_pfns(base, pfn) && distinct_pfns(pfn) && verify(base) &&
		 !mprotect(base + PROCESS_PAGE, PROCESS_PAGE,
			   PROT_READ | PROT_WRITE);
	ksft_test_result(passed,
			 "partial mprotect depacks to singleton folios\n");
	munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && !mlock(base, NATIVE_PAGE) &&
		 read_pfns(base, pfn) && distinct_pfns(pfn) && verify(base) &&
		 !munlock(base, NATIVE_PAGE);
	ksft_test_result(passed, "mlock depacks before setting VM_LOCKED\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED && !madvise(base, NATIVE_PAGE,
						  MADV_PAGEOUT) &&
		 verify(base) && read_pfns(base, pfn) && distinct_pfns(pfn);
	ksft_test_result(passed,
			 "pageout depacks safely and preserves contents\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	if (base != MAP_FAILED)
		populate(base);
	passed = base != MAP_FAILED &&
		 !madvise(base + PROCESS_PAGE, PROCESS_PAGE, MADV_DONTNEED) &&
		 base[0] == 0x31 && base[PROCESS_PAGE] == 0 &&
		 base[2 * PROCESS_PAGE] == 0x33 &&
		 base[3 * PROCESS_PAGE] == 0x34;
	ksft_test_result(passed,
			 "partial MADV_DONTNEED affects only requested slice\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	ksft_test_result(concurrent_partial_madvise(),
			 "partial MADV_DONTNEED excludes concurrent write faults\n");

	uffd_move = userfaultfd_move_depacks();
	if (!uffd_move.supported) {
		ksft_test_result_skip("UFFDIO_MOVE is unavailable\n");
		ksft_test_result_skip("UFFDIO_MOVE is unavailable\n");
		ksft_test_result_skip("UFFDIO_MOVE is unavailable\n");
	} else {
		ksft_test_result(uffd_move.register_depacked,
				 "userfaultfd registration depacks anonymous tuples\n");
		ksft_test_result(uffd_move.move_preserved,
				 "single-slice UFFDIO_MOVE preserves tuple data\n");
		ksft_test_result(uffd_move.unregistered_source_preserved,
				 "UFFDIO_MOVE depacks an unregistered source tuple\n");
	}

	base = map_aligned(2 * NATIVE_PAGE, &reservation);
	process_vm_read_ok = false;
	process_vm_write_ok = false;
	if (base != MAP_FAILED)
		process_vm_rw_preserves_packed_slices(base, &process_vm_read_ok,
						      &process_vm_write_ok);
	ksft_test_result(process_vm_read_ok,
			 "process_vm_readv preserves all packed slices\n");
	ksft_test_result(process_vm_write_ok,
			 "process_vm_writev preserves all packed slices\n");
	if (base != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE);

	ksft_test_result(mremap_handles_packed_tuple(true),
			 "same-slice mremap preserves a complete packed tuple\n");
	ksft_test_result(mremap_handles_packed_tuple(false),
			 "cross-slice mremap depacks a complete tuple\n");

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
	execl("/proc/self/exe", "packed_anon_ppps", "--run", NULL);
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
