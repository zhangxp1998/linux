// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE		4096UL
#define NATIVE_PAGE_SIZE	16384UL
#define RESERVE_SIZE		(8 * NATIVE_PAGE_SIZE)

static int perf_event_open(struct perf_event_attr *attr)
{
	return syscall(__NR_perf_event_open, attr, 0, -1, -1, 0);
}

static void copy_ring(void *destination, const unsigned char *data,
		      size_t data_size, uint64_t position, size_t length)
{
	size_t offset = position % data_size;
	size_t first = data_size - offset;

	if (first > length)
		first = length;
	memcpy(destination, data + offset, first);
	if (first < length)
		memcpy((unsigned char *)destination + first, data,
		       length - first);
}

static bool find_phys_addr(struct perf_event_mmap_page *metadata,
			   uint64_t expected_addr, uint64_t *phys_addr)
{
	size_t data_offset = metadata->data_offset ?: USER_PAGE_SIZE;
	size_t data_size = metadata->data_size ?: USER_PAGE_SIZE;
	const unsigned char *data = (const unsigned char *)metadata + data_offset;
	uint64_t head = __atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE);
	uint64_t position = metadata->data_tail;
	bool found = false;

	while (position < head) {
		struct perf_event_header header;

		copy_ring(&header, data, data_size, position, sizeof(header));
		if (header.size < sizeof(header) || header.size > data_size)
			break;
		if (header.type == PERF_RECORD_SAMPLE &&
		    header.size >= sizeof(header) + 2 * sizeof(uint64_t)) {
			uint64_t sample[2];

			copy_ring(sample, data, data_size,
				  position + sizeof(header), sizeof(sample));
			if (sample[0] == expected_addr) {
				*phys_addr = sample[1];
				found = true;
			}
		}
		position += header.size;
	}
	__atomic_store_n(&metadata->data_tail, position, __ATOMIC_RELEASE);
	return found;
}

static int sample_fault_phys_addr(unsigned char *address,
				  uint64_t *phys_addr)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_PAGE_FAULTS_MIN,
		.sample_period = 1,
		.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR,
		.disabled = 1,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.wakeup_events = 1,
	};
	struct perf_event_mmap_page *metadata;
	int fd;
	int ret = -1;

	fd = perf_event_open(&attr);
	if (fd < 0)
		return -1;
	metadata = mmap(NULL, 2 * USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (metadata == MAP_FAILED)
		goto out_close;
	if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) ||
	    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))
		goto out_unmap;

	(void)__atomic_load_n(address, __ATOMIC_RELAXED);
	__asm__ __volatile__("" : : : "memory");
	if (ioctl(fd, PERF_EVENT_IOC_DISABLE, 0))
		goto out_unmap;
	if (find_phys_addr(metadata, (uintptr_t)address, phys_addr))
		ret = 0;

out_unmap:
	munmap(metadata, 2 * USER_PAGE_SIZE);
out_close:
	close(fd);
	return ret;
}

static int run_test(void)
{
	unsigned char *alias0 = MAP_FAILED;
	unsigned char *alias1 = MAP_FAILED;
	void *reservation;
	uintptr_t base;
	uint64_t phys0 = 0;
	uint64_t phys1 = 0;
	bool mappings_ok;
	int memfd;
	int sample0;
	int sample1;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	memfd = memfd_create("perf-phys-addr-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, USER_PAGE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_msg("address reservation failed: %s\n",
				   strerror(errno));
	base = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		~(NATIVE_PAGE_SIZE - 1);
	munmap(reservation, RESERVE_SIZE);

	alias0 = mmap((void *)base, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED_NOREPLACE, memfd, 0);
	alias1 = mmap((void *)(base + NATIVE_PAGE_SIZE + USER_PAGE_SIZE),
		      USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED_NOREPLACE, memfd, 0);
	mappings_ok = alias0 != MAP_FAILED && alias1 != MAP_FAILED;
	ksft_test_result(mappings_ok,
			 "map the same file byte at native offsets 0 and 4K\n");
	if (!mappings_ok)
		ksft_exit_fail_msg("alias mmap failed: %s\n", strerror(errno));

	sample0 = sample_fault_phys_addr(alias0, &phys0);
	ksft_test_result(sample0 == 0 && phys0,
			 "sample a minor-fault physical address at native offset 0\n");
	sample1 = sample_fault_phys_addr(alias1, &phys1);
	ksft_test_result(sample1 == 0 && phys1,
			 "sample a minor-fault physical address at native offset 4K\n");
	__atomic_store_n(alias0, 0x5a, __ATOMIC_RELAXED);
	ksft_test_result(__atomic_load_n(alias1, __ATOMIC_RELAXED) == 0x5a,
			 "aliases resolve to the same file byte\n");
	ksft_print_msg("phys0=%#llx phys1=%#llx delta=%lld\n",
		       (unsigned long long)phys0, (unsigned long long)phys1,
		       (long long)(phys1 - phys0));
	ksft_test_result(sample0 == 0 && sample1 == 0 && phys0 == phys1,
			 "PERF_SAMPLE_PHYS_ADDR identifies the same physical byte\n");

	munmap(alias1, USER_PAGE_SIZE);
	munmap(alias0, USER_PAGE_SIZE);
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
	execl("/proc/self/exe", "perf_phys_addr_ppps", "--run", NULL);
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
