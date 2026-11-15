// SPDX-License-Identifier: GPL-2.0
/* BPF map mmap is native-only, including FDs inherited by a 4KB process. */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

static const enum bpf_map_type types[] = {
	BPF_MAP_TYPE_ARRAY, BPF_MAP_TYPE_RINGBUF, BPF_MAP_TYPE_USER_RINGBUF,
};
static int map_fds[3];
static const uint64_t marker = 0x1234567887654321ULL;

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

static bool check_mapping(int fd, size_t size, int prot, off_t offset,
			  int native_errno, const uint64_t *expected)
{
	bool compat = p3s_is_compat_process();
	int wanted = compat ? EOPNOTSUPP : native_errno;
	void *p;
	bool ok;

	errno = 0;
	p = mmap(NULL, size, prot, MAP_SHARED, fd, offset);
	if (wanted) {
		ok = p == MAP_FAILED && errno == wanted;
	} else {
		ok = p != MAP_FAILED;
		if (ok && expected)
			ok = *(uint64_t *)p == *expected;
	}
	if (!ok)
		ksft_print_msg("mmap size=%zu prot=%#x offset=%lld: %p errno=%d expected=%d\n",
			       size, prot, (long long)offset, p, errno, wanted);
	if (p != MAP_FAILED)
		munmap(p, size);
	return ok;
}

static int update_array(uint64_t value)
{
	unsigned int key = 0;
	union bpf_attr attr = {
		.map_fd = map_fds[0], .key = (unsigned long)&key,
		.value = (unsigned long)&value, .flags = BPF_ANY,
	};

	return sys_bpf(BPF_MAP_UPDATE_ELEM, &attr);
}

TEST(array_update_and_lookup_without_mmap)
{
	uint64_t value = 0;
	unsigned int key = 0;
	union bpf_attr attr = {
		.map_fd = map_fds[0], .key = (unsigned long)&key,
		.value = (unsigned long)&value, .flags = BPF_ANY,
	};

	ASSERT_EQ(0, update_array(marker));
	ASSERT_EQ(0, sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr));
	EXPECT_EQ(marker, value);
}

TEST(array_mmap)
{
	ASSERT_EQ(0, update_array(marker));
	EXPECT_TRUE(check_mapping(map_fds[0], getpagesize(),
				  PROT_READ | PROT_WRITE, 0, 0, &marker));
}

TEST(array_mmap_out_of_bounds)
{
	size_t page = getpagesize();

	EXPECT_TRUE(check_mapping(map_fds[0], page, PROT_READ, page,
				  EINVAL, NULL));
}

TEST(kernel_ring_consumer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[1], getpagesize(),
				  PROT_READ | PROT_WRITE, 0, 0, NULL));
}

TEST(kernel_ring_producer_mmap)
{
	size_t page = getpagesize();

	EXPECT_TRUE(check_mapping(map_fds[1], page, PROT_READ, page, 0, NULL));
}

TEST(kernel_ring_data_alias_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[1], 2 * NATIVE_PAGE_SIZE, PROT_READ,
				  2 * getpagesize(), 0, NULL));
}

TEST(kernel_ring_writable_producer_is_rejected)
{
	size_t page = getpagesize();

	EXPECT_TRUE(check_mapping(map_fds[1], page, PROT_READ | PROT_WRITE,
				  page, EPERM, NULL));
}

TEST(user_ring_consumer_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[2], getpagesize(), PROT_READ,
				  0, 0, NULL));
}

TEST(user_ring_producer_mmap)
{
	size_t page = getpagesize();

	EXPECT_TRUE(check_mapping(map_fds[2], page, PROT_READ | PROT_WRITE,
				  page, 0, NULL));
}

TEST(user_ring_data_alias_mmap)
{
	EXPECT_TRUE(check_mapping(map_fds[2], 2 * NATIVE_PAGE_SIZE,
				  PROT_READ | PROT_WRITE, 2 * getpagesize(), 0, NULL));
}

TEST(user_ring_writable_consumer_is_rejected)
{
	EXPECT_TRUE(check_mapping(map_fds[2], getpagesize(),
				  PROT_READ | PROT_WRITE, 0, EPERM, NULL));
}

TEST(kernel_ring_size_4k_is_rejected)
{
	int fd;

	errno = 0;
	fd = create_map(BPF_MAP_TYPE_RINGBUF, PROCESS_PAGE_SIZE);
	EXPECT_EQ(-1, fd);
	EXPECT_EQ(EINVAL, errno);
	if (fd >= 0)
		close(fd);
}

TEST(kernel_ring_size_8k_is_rejected)
{
	int fd;

	errno = 0;
	fd = create_map(BPF_MAP_TYPE_RINGBUF, 2 * PROCESS_PAGE_SIZE);
	EXPECT_EQ(-1, fd);
	EXPECT_EQ(EINVAL, errno);
	if (fd >= 0)
		close(fd);
}

int main(int argc, char **argv)
{
	const char *saved = getenv("P3S_BPF_NATIVE_FDS");
	char buffer[96];
	int i;

	if (saved) {
		if (sscanf(saved, "%d,%d,%d", &map_fds[0], &map_fds[1],
			   &map_fds[2]) != 3)
			ksft_exit_fail_msg("invalid inherited map FDs\n");
	} else if (getpagesize() == NATIVE_PAGE_SIZE) {
		for (i = 0; i < 3; i++) {
			map_fds[i] = create_map(types[i], i ? NATIVE_PAGE_SIZE : 1);
			if (map_fds[i] < 0) {
				if (errno == EPERM || errno == EACCES)
					ksft_exit_skip("BPF maps unavailable: %s\n", strerror(errno));
				ksft_exit_fail_msg("create map type %u: %s\n", types[i], strerror(errno));
			}
			if (fcntl(map_fds[i], F_SETFD, 0))
				ksft_exit_fail_msg("retain map FD across exec\n");
		}
		snprintf(buffer, sizeof(buffer), "%d,%d,%d", map_fds[0],
			 map_fds[1], map_fds[2]);
		if (setenv("P3S_BPF_NATIVE_FDS", buffer, 1))
			ksft_exit_fail_msg("save map FDs\n");
	}
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
