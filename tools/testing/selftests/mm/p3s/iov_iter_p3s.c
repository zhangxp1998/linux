// SPDX-License-Identifier: GPL-2.0
/*
 * The in-kernel iov_iter page extraction helpers, driven by the
 * iov_iter_p3s fixture on a 4KB process's buffers, return the exact
 * 4KB slices for a file mapping at a mismatched native offset and for a
 * packed anonymous native page, and bulk extraction preserves consecutive
 * physical slice runs.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "iov_iter_p3s.h"
#include "page_range_p3s.h"

#define FILE_TEST_LENGTH (2 * PROCESS_PAGE_SIZE)
#define ANON_TEST_LENGTH NATIVE_PAGE_SIZE
#define PHYS_SLICE_TEST_LENGTH NATIVE_PAGE_SIZE

static const unsigned char expected[] = { 0x31, 0x72, 0x93, 0xb4 };

static void *map_test_file(int *fd_out, void **reservation_out)
{
	const char *tmpdir = getenv("TMPDIR");
	char path[PATH_MAX];
	uintptr_t aligned;
	void *reservation;
	void *mapping;
	int fd, len;

	if (!tmpdir || !tmpdir[0])
		tmpdir = "/tmp";
	len = snprintf(path, sizeof(path), "%s/iov-iter-XXXXXX", tmpdir);
	if (len < 0 || len >= (int)sizeof(path))
		return MAP_FAILED;

	fd = mkstemp(path);
	if (fd < 0)
		return MAP_FAILED;
	unlink(path);
	if (ftruncate(fd, NATIVE_PAGE_SIZE))
		goto err;

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto err;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)(aligned + PROCESS_PAGE_SIZE), FILE_TEST_LENGTH,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
		goto err;
	}

	*fd_out = fd;
	*reservation_out = reservation;
	return mapping;

err:
	close(fd);
	return MAP_FAILED;
}

static unsigned char *map_test_anon(void **reservation_out)
{
	uintptr_t aligned;
	void *reservation;

	reservation = mmap(NULL, 2 * NATIVE_PAGE_SIZE,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	*reservation_out = reservation;
	return (unsigned char *)aligned;
}

static void populate(unsigned char *mapping, size_t length)
{
	unsigned int i;

	for (i = 0; i < length / PROCESS_PAGE_SIZE; i++)
		mapping[i * PROCESS_PAGE_SIZE] = expected[i];
}

enum mapping_kind { FILE_SLICES, PACKED_ANON, NONLINEAR_PHYS };

FIXTURE(iov_iter) {
	unsigned char *mapping;
	void *reservation;
	int backing_fd;
	int device_fd;
};

FIXTURE_VARIANT(iov_iter) {
	enum mapping_kind kind;
	size_t length;
	size_t first_length;
	unsigned int flags;
};

FIXTURE_VARIANT_ADD(iov_iter, file_slices) {
	.kind = FILE_SLICES, .length = FILE_TEST_LENGTH,
	.first_length = FILE_TEST_LENGTH,
};
FIXTURE_VARIANT_ADD(iov_iter, packed_anon) {
	.kind = PACKED_ANON, .length = ANON_TEST_LENGTH,
	.first_length = ANON_TEST_LENGTH,
	.flags = IOV_ITER_P3S_F_EXPECT_PACKED,
};
FIXTURE_VARIANT_ADD(iov_iter, nonlinear_phys) {
	.kind = NONLINEAR_PHYS, .length = PHYS_SLICE_TEST_LENGTH,
	.first_length = PROCESS_PAGE_SIZE,
};

FIXTURE_SETUP(iov_iter)
{
	self->backing_fd = -1;
	self->device_fd = p3s_open_fixture("/dev/" IOV_ITER_P3S_DEVICE_NAME,
					   O_RDWR);
	if (self->device_fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "iov_iter fixture is unavailable");
	ASSERT_GE(self->device_fd, 0);
	switch (variant->kind) {
	case FILE_SLICES:
		self->mapping = map_test_file(&self->backing_fd,
					      &self->reservation);
		break;
	case PACKED_ANON:
		self->mapping = map_test_anon(&self->reservation);
		break;
	case NONLINEAR_PHYS:
		self->mapping = page_range_map(self->device_fd);
		break;
	}
	ASSERT_NE(self->mapping, MAP_FAILED);
	populate(self->mapping, variant->length);
}

FIXTURE_TEARDOWN(iov_iter)
{
	switch (variant->kind) {
	case FILE_SLICES:
		munmap(self->reservation, 3 * NATIVE_PAGE_SIZE);
		close(self->backing_fd);
		break;
	case PACKED_ANON:
		munmap(self->reservation, 2 * NATIVE_PAGE_SIZE);
		break;
	case NONLINEAR_PHYS:
		munmap(self->mapping, PHYS_SLICE_TEST_LENGTH);
		break;
	}
	close(self->device_fd);
}

static bool invoke_iov(int fd, const void *mapping, size_t length,
		       unsigned int flags, struct iov_iter_p3s_args *request)
{
	*request = (struct iov_iter_p3s_args) {
		.address = (uintptr_t)mapping,
		.length = length,
		.flags = flags,
	};
	return ioctl(fd, IOV_ITER_P3S_IOCTL, request) == 0;
}

TEST_F(iov_iter, get_pages_returns_exact_slices)
{
	struct iov_iter_p3s_args r;

	ASSERT_TRUE(invoke_iov(self->device_fd, self->mapping, variant->length,
			       variant->flags, &r));
	EXPECT_EQ(0, r.get_pages_result);
}

TEST_F(iov_iter, extract_pages_returns_exact_slices)
{
	struct iov_iter_p3s_args r;

	ASSERT_TRUE(invoke_iov(self->device_fd, self->mapping, variant->length,
			       variant->flags, &r));
	EXPECT_EQ(0, r.extract_pages_result);
}

TEST_F(iov_iter, bulk_extraction_preserves_contiguous_run)
{
	struct iov_iter_p3s_args r;

	ASSERT_TRUE(invoke_iov(self->device_fd, self->mapping, variant->length,
			       variant->flags, &r));
	EXPECT_EQ(variant->first_length, r.bulk_first_len);
}

TEST_F(iov_iter, npages_counts_and_caps_process_pages)
{
	struct iov_iter_p3s_args r;

	ASSERT_TRUE(invoke_iov(self->device_fd, self->mapping, variant->length,
			       variant->flags, &r));
	EXPECT_EQ(0, r.npages_result);
}

P3S_HARNESS_4KB_MAIN
