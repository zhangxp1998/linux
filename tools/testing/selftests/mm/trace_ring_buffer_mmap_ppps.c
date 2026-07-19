// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/trace_mmap.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE	4096UL
#define TRACE_PIPE_RAW	"/sys/kernel/tracing/per_cpu/cpu0/trace_pipe_raw"

static int run_test(void)
{
	struct trace_buffer_meta *meta;
	unsigned long data_len;
	unsigned long meta_len;
	unsigned long offset;
	unsigned char checksum = 0;
	unsigned char *data;
	void *subpage;
	bool layout_ok;
	bool subpage_ok;
	int saved_errno;
	int fd;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open(TRACE_PIPE_RAW, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV)
			ksft_exit_skip("tracefs ring buffer is unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("open %s failed: %s\n", TRACE_PIPE_RAW,
				   strerror(errno));
	}
	ksft_test_result(true, "open the CPU0 trace ring buffer\n");

	meta = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (meta == MAP_FAILED)
		ksft_exit_fail_msg("metadata mmap failed: %s\n", strerror(errno));
	ksft_test_result(true, "map the ring-buffer metadata page\n");

	meta_len = meta->meta_page_size;
	data_len = (unsigned long)meta->subbuf_size * meta->nr_subbufs;
	layout_ok = meta->meta_struct_len >= sizeof(*meta) &&
		meta->meta_struct_len <= meta_len &&
		meta_len >= USER_PAGE_SIZE && !(meta_len % USER_PAGE_SIZE) &&
		meta->subbuf_size && meta->nr_subbufs && data_len;
	ksft_test_result(layout_ok, "metadata describes a valid native-page ring\n");
	ksft_print_msg("meta=%lu struct=%u subbuf=%u nr_subbufs=%u data=%lu\n",
		       meta_len, meta->meta_struct_len, meta->subbuf_size,
		       meta->nr_subbufs, data_len);
	if (!layout_ok)
		ksft_exit_fail_msg("invalid trace ring-buffer metadata\n");

	errno = 0;
	subpage = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       USER_PAGE_SIZE);
	saved_errno = errno;
	subpage_ok = meta_len == USER_PAGE_SIZE ? subpage != MAP_FAILED :
					       subpage == MAP_FAILED;
	ksft_test_result(subpage_ok,
			 "handle a 4K mmap offset according to the native-page layout (errno=%d)\n",
			 saved_errno);
	if (subpage != MAP_FAILED)
		munmap(subpage, USER_PAGE_SIZE);

	errno = 0;
	data = mmap(NULL, data_len, PROT_READ, MAP_SHARED, fd, meta_len);
	saved_errno = errno;
	ksft_test_result(data != MAP_FAILED,
			 "map the complete ring using 4K process pages\n");
	ksft_print_msg("data mmap=%p errno=%d\n", data, saved_errno);
	if (data == MAP_FAILED)
		ksft_exit_fail_msg("full data mmap failed: %s\n",
				   strerror(saved_errno));

	for (offset = 0; offset < data_len; offset += USER_PAGE_SIZE)
		checksum ^= data[offset];
	ksft_test_result(true, "access every mapped 4K slice without SIGBUS\n");
	ksft_print_msg("mapped data checksum=%u\n", checksum);

	errno = 0;
	ksft_test_result(ioctl(fd, TRACE_MMAP_IOCTL_GET_READER) == 0,
			 "swap a reader sub-buffer through the mmap ioctl\n");

	munmap(data, data_len);
	munmap(meta, USER_PAGE_SIZE);
	close(fd);
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
	execl("/proc/self/exe", "trace_ring_buffer_mmap_ppps", "--run", NULL);
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
