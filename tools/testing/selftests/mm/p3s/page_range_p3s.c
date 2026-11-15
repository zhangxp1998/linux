// SPDX-License-Identifier: GPL-2.0
/* Byte spans through exported iov APIs, including short results. */
#define _GNU_SOURCE
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include "p3s_test.h"
#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "iov_iter_p3s.h"
#include "page_range_p3s.h"

/* Driver-owned RAM has no page-cache rmap: its fallback is conservative. */
static bool driver_stats(unsigned char *map, size_t page_size,
			 unsigned long expected_pss, unsigned int exclusive_mask)
{
	unsigned long pss;
	bool ok = true;
	uint64_t entry = 0;
	int i;

	if (!p3s_smaps_bytes(map, 4 * page_size, "Pss", &pss))
		return false;
	pss /= 1024;
	for (i = 0; i < 4; i++) {
		ok &= p3s_pagemap_entry(map + i * page_size, &entry);
		ok &= !!(entry & PM_PRESENT) &&
		      !!(entry & PM_MMAP_EXCLUSIVE) ==
		      !!(exclusive_mask & (1U << i));
	}
	return ok && pss == expected_pss;
}

static int open_fixture(void)
{
	return p3s_open_fixture("/dev/" IOV_ITER_P3S_DEVICE_NAME, O_RDWR);
}

FIXTURE(range_bytes) {
	int fd;
	int memfd;
	unsigned char *anon;
	unsigned char *file;
	size_t page;
};

FIXTURE_VARIANT(range_bytes) {
	bool file;
	bool pin;
};

FIXTURE_VARIANT_ADD(range_bytes, anonymous_get) { };
FIXTURE_VARIANT_ADD(range_bytes, anonymous_pin) { .pin = true };
FIXTURE_VARIANT_ADD(range_bytes, file_get) { .file = true };
FIXTURE_VARIANT_ADD(range_bytes, file_pin) { .file = true, .pin = true };

FIXTURE_SETUP(range_bytes)
{
	self->page = getpagesize();
	self->fd = open_fixture();
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "iov_iter fixture is unavailable");
	ASSERT_GE(self->fd, 0);
	self->memfd = memfd_create("page-range", 0);
	ASSERT_GE(self->memfd, 0);
	ASSERT_EQ(0, ftruncate(self->memfd, 3 * NATIVE_PAGE_SIZE));
	self->anon = mmap(NULL, 3 * NATIVE_PAGE_SIZE,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	self->file = mmap(NULL, 2 * NATIVE_PAGE_SIZE,
			  PROT_READ | PROT_WRITE,
			  MAP_SHARED, self->memfd, self->page);
	ASSERT_NE(self->anon, MAP_FAILED);
	ASSERT_NE(self->file, MAP_FAILED);
	memset(self->anon, 0x31, self->page);
	memset(self->anon + self->page, 0x72, self->page);
	memset(self->file, 0x31, self->page);
	memset(self->file + self->page, 0x72, self->page);
}

FIXTURE_TEARDOWN(range_bytes)
{
	munmap(self->anon, 3 * NATIVE_PAGE_SIZE);
	munmap(self->file, 2 * NATIVE_PAGE_SIZE);
	close(self->memfd);
	close(self->fd);
}

TEST_F(range_bytes, exact_byte_spans)
{
	unsigned char *base = variant->file ? self->file : self->anon;
	size_t backing_offset = variant->file ?
		self->page % NATIVE_PAGE_SIZE : 0;
	struct page_range_p3s_args req = {
		.address = (uintptr_t)base + 17,
		.length = self->page,
		.capacity = PAGE_RANGE_P3S_MAX,
		.pin = variant->pin,
	};

	ASSERT_EQ(0, ioctl(self->fd, PAGE_RANGE_P3S_IOCTL, &req));
	ASSERT_EQ(2, req.result);
	EXPECT_EQ(backing_offset + 17, req.spans[0].offset);
	EXPECT_EQ(self->page - 17, req.spans[0].length);
	EXPECT_EQ(0x31, req.spans[0].first);
	EXPECT_EQ(0x31, req.spans[0].last);
	EXPECT_EQ((backing_offset + self->page) % NATIVE_PAGE_SIZE,
		  req.spans[1].offset);
	EXPECT_EQ(17, req.spans[1].length);
	EXPECT_EQ(0x72, req.spans[1].first);
	EXPECT_EQ(0x72, req.spans[1].last);
}

FIXTURE(range_edges) {
	int fd;
	unsigned char *anon;
	size_t page;
};

FIXTURE_SETUP(range_edges)
{
	self->page = getpagesize();
	self->fd = open_fixture();
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "iov_iter fixture is unavailable");
	ASSERT_GE(self->fd, 0);
	self->anon = mmap(NULL, 3 * NATIVE_PAGE_SIZE,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->anon, MAP_FAILED);
	memset(self->anon, 0x31, self->page);
	memset(self->anon + self->page, 0x72, self->page);
}

FIXTURE_TEARDOWN(range_edges)
{
	munmap(self->anon, 3 * NATIVE_PAGE_SIZE);
	close(self->fd);
}

static bool short_range(const struct _test_data_range_edges *self, bool pin)
{
	struct page_range_p3s_args req = {
		.address = (uintptr_t)self->anon + 17,
		.length = self->page,
		.capacity = 2,
		.pin = pin,
	};

	if (mprotect(self->anon + self->page, self->page, PROT_NONE) ||
	    ioctl(self->fd, PAGE_RANGE_P3S_IOCTL, &req))
		return false;
	return req.result == 1 && req.spans[0].length == self->page - 17 &&
	       req.spans[0].offset == 17;
}

TEST_F(range_edges, get_short_result_describes_accessible_bytes)
{
	EXPECT_TRUE(short_range(self, false));
}

TEST_F(range_edges, pin_short_result_describes_accessible_bytes)
{
	EXPECT_TRUE(short_range(self, true));
}

FIXTURE(driver_mapping) {
	int fd;
	unsigned char *mapping;
	size_t page;
};

FIXTURE_SETUP(driver_mapping)
{
	self->page = getpagesize();
	self->fd = open_fixture();
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "iov_iter fixture is unavailable");
	ASSERT_GE(self->fd, 0);
	self->mapping = page_range_map(self->fd);
	ASSERT_NE(self->mapping, MAP_FAILED);
}

FIXTURE_TEARDOWN(driver_mapping)
{
	munmap(self->mapping, 4 * self->page);
	close(self->fd);
}

static bool driver_spans(const struct _test_data_driver_mapping *self,
			 bool pin)
{
	static const unsigned int slices[] = { 3, 1, 0, 2 };
	static const unsigned char values[] = { 0x31, 0x72, 0x93, 0xb4 };
	bool compat = self->page == PROCESS_PAGE_SIZE;
	struct page_range_p3s_args req = {
		.address = (uintptr_t)self->mapping,
		.length = 4 * self->page,
		.capacity = 4,
		.pin = pin,
	};

	if (ioctl(self->fd, PAGE_RANGE_P3S_IOCTL, &req) || req.result != 4)
		return false;
	for (unsigned int i = 0; i < 4; i++) {
		unsigned int idx = compat ? slices[i] : i;

		if (req.spans[i].offset != (compat ? idx * self->page : 0) ||
		    req.spans[i].length != self->page ||
		    req.spans[i].first != values[idx] ||
		    req.spans[i].last != values[idx])
			return false;
	}
	return true;
}

TEST_F(driver_mapping, get_follows_selected_physical_slices)
{
	EXPECT_TRUE(driver_spans(self, false));
}

TEST_F(driver_mapping, pin_follows_selected_physical_slices)
{
	EXPECT_TRUE(driver_spans(self, true));
}

TEST_F(driver_mapping, process_vm_readv_preserves_slice_order)
{
	size_t size = 4 * self->page;
	unsigned char *copy = malloc(size);
	struct iovec local = { copy, size };
	struct iovec remote = { self->mapping, size };

	ASSERT_NE(copy, NULL);
	EXPECT_TRUE(process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == size &&
		    !memcmp(copy, self->mapping, size));
	free(copy);
}

TEST_F(driver_mapping, process_vm_writev_preserves_slice_order)
{
	size_t size = 4 * self->page;
	unsigned char *copy = malloc(size);
	struct iovec local = { copy, size };
	struct iovec remote = { self->mapping, size };

	ASSERT_NE(copy, NULL);
	for (unsigned int i = 0; i < 4; i++)
		memset(copy + i * self->page, 0xc0 + i, self->page);
	EXPECT_TRUE(process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == size &&
		    !memcmp(copy, self->mapping, size));
	free(copy);
}

TEST_F(driver_mapping, self_mem_read_preserves_slice_order)
{
	size_t size = 4 * self->page;
	unsigned char *copy = malloc(size);
	int mem = open("/proc/self/mem", O_RDWR);

	ASSERT_NE(copy, NULL);
	ASSERT_GE(mem, 0);
	EXPECT_TRUE(pread(mem, copy, size, (uintptr_t)self->mapping) == size &&
		    !memcmp(copy, self->mapping, size));
	close(mem);
	free(copy);
}

TEST_F(driver_mapping, self_mem_write_preserves_slice_order)
{
	size_t size = 4 * self->page;
	unsigned char *copy = malloc(size);
	int mem = open("/proc/self/mem", O_RDWR);

	ASSERT_NE(copy, NULL);
	ASSERT_GE(mem, 0);
	for (unsigned int i = 0; i < 4; i++)
		memset(copy + i * self->page, 0xd0 + i, self->page);
	EXPECT_TRUE(pwrite(mem, copy, size, (uintptr_t)self->mapping) == size &&
		    !memcmp(copy, self->mapping, size));
	close(mem);
	free(copy);
}

static unsigned long driver_pss(const struct _test_data_driver_mapping *self)
{
	return 4 * self->page / 1024;
}

static unsigned int exclusive_mask(const struct _test_data_driver_mapping *self)
{
	return self->page == PROCESS_PAGE_SIZE ? 0x2 : 0xf;
}

TEST_F(driver_mapping, ram_uses_native_sharing_and_window_exclusivity)
{
	EXPECT_TRUE(driver_stats(self->mapping, self->page, driver_pss(self),
				 exclusive_mask(self)));
}

static bool forked_driver_stats(const struct _test_data_driver_mapping *self,
				bool after_exit)
{
	int barrier[2], status;
	pid_t child;
	char byte;
	bool shared;

	if (pipe(barrier))
		return false;
	child = fork();
	if (!child) {
		close(barrier[1]);
		_exit(read(barrier[0], &byte, 1) != 1);
	}
	if (child < 0) {
		close(barrier[0]);
		close(barrier[1]);
		return false;
	}
	close(barrier[0]);
	shared = driver_stats(self->mapping, self->page,
			      driver_pss(self) / 2, 0);
	if (write(barrier[1], "x", 1) != 1 ||
	    waitpid(child, &status, 0) != child) {
		close(barrier[1]);
		return false;
	}
	close(barrier[1]);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return false;
	return after_exit ?
		driver_stats(self->mapping, self->page, driver_pss(self),
			     exclusive_mask(self)) : shared;
}

TEST_F(driver_mapping, shared_ram_is_never_exclusive)
{
	EXPECT_TRUE(forked_driver_stats(self, false));
}

TEST_F(driver_mapping, ram_accounting_recovers_after_child_exit)
{
	EXPECT_TRUE(forked_driver_stats(self, true));
}

P3S_HARNESS_4KB_MAIN
