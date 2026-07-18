// SPDX-License-Identifier: GPL-2.0
/*
 * perf ring buffers of a 4K compat process are laid out, bounds-checked and
 * wrapped in process-page units, and PERF_RECORD_MMAP reports the 4K file
 * slice offset of a compat mapping.
 */
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
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define FILE_SIZE	(4 * USER_PAGE_SIZE)
#define FILE_OFFSET	USER_PAGE_SIZE
#define DATA_PAGES	8

struct mmap_record {
	struct perf_event_header header;
	uint32_t pid;
	uint32_t tid;
	uint64_t addr;
	uint64_t len;
	uint64_t pgoff;
	char filename[];
};

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

static bool find_mmap_offset(struct perf_event_mmap_page *metadata,
			     size_t page_size, uint64_t *file_offset)
{
	size_t data_offset = metadata->data_offset ?: page_size;
	size_t data_size = metadata->data_size ?: DATA_PAGES * page_size;
	const unsigned char *data = (const unsigned char *)metadata + data_offset;
	uint64_t head = __atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE);
	uint64_t position = metadata->data_tail;
	bool found = false;

	while (position < head) {
		struct perf_event_header header;
		struct mmap_record *record;

		copy_ring(&header, data, data_size, position, sizeof(header));
		if (header.size < sizeof(header) || header.size > data_size)
			break;
		record = malloc(header.size);
		if (!record)
			break;
		copy_ring(record, data, data_size, position, header.size);
		if (record->header.type == PERF_RECORD_MMAP &&
		    record->header.size > sizeof(*record) &&
		    strstr(record->filename, "perf-mmap-offset-ppps")) {
			*file_offset = record->pgoff;
			found = true;
		}
		position += header.size;
		free(record);
		if (found)
			break;
	}
	__atomic_store_n(&metadata->data_tail, position, __ATOMIC_RELEASE);
	return found;
}

static int open_perf_ring(struct perf_event_attr *attr,
			  struct perf_event_mmap_page **metadata,
			  size_t *page_size, size_t *ring_size)
{
	int fd = perf_event_open(attr);

	if (fd < 0)
		return -1;
	*page_size = USER_PAGE_SIZE;
	*ring_size = (DATA_PAGES + 1) * *page_size;
	*metadata = mmap(NULL, *ring_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	if (*metadata != MAP_FAILED)
		return fd;
	close(fd);
	return -1;
}

static bool map_metadata_only(struct perf_event_attr *attr)
{
	struct perf_event_mmap_page *metadata;
	int fd;

	fd = perf_event_open(attr);
	if (fd < 0)
		return false;
	metadata = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (metadata == MAP_FAILED) {
		close(fd);
		return false;
	}
	munmap(metadata, PROCESS_PAGE_SIZE);
	close(fd);
	return true;
}

static int run_test(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_DUMMY,
		.disabled = 1,
		.mmap = 1,
		.mmap_data = 1,
	};
	struct perf_event_mmap_page *metadata = MAP_FAILED;
	uint64_t recorded_offset = UINT64_MAX;
	size_t page_size = 0;
	size_t ring_size = 0;
	size_t data_offset;
	size_t data_size;
	void *file_mapping;
	bool found;
	int memfd;
	int perf_fd;

	ksft_print_header();
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ksft_test_result(map_metadata_only(&attr),
			 "map a metadata-only perf event with one process page\n");

	perf_fd = open_perf_ring(&attr, &metadata, &page_size, &ring_size);
	ksft_test_result(page_size && perf_fd >= 0 && metadata != MAP_FAILED,
			 "open and map a software perf event\n");
	if (!page_size || perf_fd < 0 || metadata == MAP_FAILED)
		ksft_exit_fail_msg("perf event setup failed: %s\n",
				   strerror(errno));
	data_offset = metadata->data_offset ?: page_size;
	data_size = metadata->data_size ?: DATA_PAGES * page_size;
	ksft_print_msg("ring_size=%zu data_offset=%zu data_size=%zu\n",
		       ring_size, data_offset, data_size);
	ksft_test_result(data_offset <= ring_size &&
			 data_size <= ring_size - data_offset,
			 "keep the effective perf data ring inside the VMA\n");
	if (ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0))
		ksft_exit_fail_msg("perf enable failed: %s\n", strerror(errno));

	memfd = memfd_create("perf-mmap-offset-ppps", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, FILE_SIZE))
		ksft_exit_fail_msg("memfd setup failed: %s\n", strerror(errno));
	file_mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC,
			    MAP_PRIVATE, memfd, FILE_OFFSET);
	ksft_test_result(file_mapping != MAP_FAILED,
			 "map one 4K page at file offset 4K\n");
	if (file_mapping == MAP_FAILED)
		ksft_exit_fail_msg("file mmap failed: %s\n", strerror(errno));

	ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
	found = find_mmap_offset(metadata, page_size, &recorded_offset);
	ksft_test_result(found, "find the target PERF_RECORD_MMAP\n");
	ksft_print_msg("recorded pgoff=%#llx expected=%#lx\n",
		       (unsigned long long)recorded_offset, FILE_OFFSET);
	ksft_test_result(found && recorded_offset == FILE_OFFSET,
			 "perf reports the PPPS file slice offset\n");

	munmap(file_mapping, PROCESS_PAGE_SIZE);
	close(memfd);
	munmap(metadata, ring_size);
	close(perf_fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
