// SPDX-License-Identifier: GPL-2.0
/* BPF map mmap returns EOPNOTSUPP in a 4KB process, including inherited FDs. */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define P3S_HARNESS_LAUNCH prepare_harness
#include "p3s_harness.h"

static const enum bpf_map_type types[] = {
	BPF_MAP_TYPE_ARRAY, BPF_MAP_TYPE_RINGBUF, BPF_MAP_TYPE_USER_RINGBUF,
};
static int map_fds[3];

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int create_map(enum bpf_map_type type, unsigned int size)
{
	union bpf_attr attr = { .map_type = type, .max_entries = size };

	if (type == BPF_MAP_TYPE_ARRAY) {
		attr.key_size = sizeof(unsigned int);
		attr.value_size = sizeof(uint64_t);
		attr.map_flags = BPF_F_MMAPABLE;
	}
	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static bool check_mapping(int fd, size_t size, int prot, off_t offset)
{
	void *p;
	int err;
	bool ok;

	errno = 0;
	p = mmap(NULL, size, prot, MAP_SHARED, fd, offset);
	err = errno;
	ok = p == MAP_FAILED && err == EOPNOTSUPP;
	if (!ok)
		ksft_print_msg("mmap size=%zu prot=%#x offset=%lld: %p errno=%d expected=%d\n",
			       size, prot, (long long)offset, p, err, EOPNOTSUPP);
	if (p != MAP_FAILED)
		munmap(p, size);
	return ok;
}

TEST(array_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[0], PROCESS_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, 0));
}

TEST(array_mmap_out_of_bounds)
{
	EXPECT_TRUE(check_mapping(map_fds[0], PROCESS_PAGE_SIZE, PROT_READ,
				  PROCESS_PAGE_SIZE));
}

TEST(kernel_ring_consumer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[1], PROCESS_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, 0));
}

TEST(kernel_ring_producer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[1], PROCESS_PAGE_SIZE, PROT_READ,
				  PROCESS_PAGE_SIZE));
}

TEST(kernel_ring_data_alias_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[1], 2 * NATIVE_PAGE_SIZE, PROT_READ,
				  2 * PROCESS_PAGE_SIZE));
}

TEST(kernel_ring_writable_producer_is_rejected)
{
	EXPECT_TRUE(check_mapping(map_fds[1], PROCESS_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, PROCESS_PAGE_SIZE));
}

TEST(user_ring_consumer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[2], PROCESS_PAGE_SIZE, PROT_READ, 0));
}

TEST(user_ring_producer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[2], PROCESS_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, PROCESS_PAGE_SIZE));
}

TEST(user_ring_data_alias_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[2], 2 * NATIVE_PAGE_SIZE,
				  PROT_READ | PROT_WRITE,
				  2 * PROCESS_PAGE_SIZE));
}

TEST(user_ring_writable_consumer_is_rejected)
{
	EXPECT_TRUE(check_mapping(map_fds[2], PROCESS_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, 0));
}

static int prepare_harness(int argc, char **argv)
{
	const char *saved = getenv("P3S_BPF_NATIVE_FDS");
	char buffer[96];
	int i;

	if (p3s_harness_listing(argc, argv))
		return -1;
	if (saved) {
		if (sscanf(saved, "%d,%d,%d", &map_fds[0], &map_fds[1],
			   &map_fds[2]) != 3)
			ksft_exit_fail_msg("invalid inherited map FDs\n");
	} else if (getpagesize() == NATIVE_PAGE_SIZE) {
		for (i = 0; i < 3; i++) {
			map_fds[i] = create_map(types[i], i ? NATIVE_PAGE_SIZE : 1);
			if (map_fds[i] < 0) {
				int err = errno;

				while (i--)
					close(map_fds[i]);
				if (err == EPERM || err == EACCES)
					ksft_exit_skip("BPF maps unavailable: %s\n",
						       strerror(err));
				ksft_exit_fail_msg("create map type %u: %s\n",
						   types[i], strerror(err));
			}
			if (fcntl(map_fds[i], F_SETFD, 0))
				ksft_exit_fail_msg("retain map FD across exec\n");
		}
		snprintf(buffer, sizeof(buffer), "%d,%d,%d", map_fds[0],
			 map_fds[1], map_fds[2]);
		if (setenv("P3S_BPF_NATIVE_FDS", buffer, 1))
			ksft_exit_fail_msg("save map FDs\n");
	}
	return -1;
}

P3S_HARNESS_4KB_MAIN
