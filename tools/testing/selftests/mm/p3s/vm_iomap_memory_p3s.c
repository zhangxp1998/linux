// SPDX-License-Identifier: GPL-2.0
/*
 * vm_iomap_memory() through the vm_iomap_memory_p3s fixture maps single 4K
 * slices for a 4K compat process: adjacent guard slices stay unmapped, a
 * private PFN mapping COWs a complete 4K page, an unaligned file offset is
 * rejected by remap_pfn_range(), and a mapping starting at the buffer end is
 * rejected.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "vm_iomap_memory_p3s.h"

#define PRIVATE_RESERVATION_SIZE (3 * NATIVE_PAGE_SIZE)
#define FIRST_MARKER	0x71
#define COW_MARKER	0xa5
#define COW_OFFSET	37

static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

static bool mapping_has_values(const unsigned char *mapping,
			       unsigned long changed_offset)
{
	unsigned long offset;

	for (offset = 0; offset < PROCESS_PAGE_SIZE; offset++) {
		unsigned char expected = offset == changed_offset ?
			COW_MARKER : FIRST_MARKER;

		if (mapping[offset] != expected) {
			ksft_print_msg("offset %lu contains %#x instead of %#x\n",
				       offset, mapping[offset], expected);
			return false;
		}
	}
	return true;
}

FIXTURE(iomap) {
	int fd;
	int unaligned_fd;
};

FIXTURE_SETUP(iomap)
{
	struct sigaction action = { .sa_handler = fault_handler };

	self->fd = -1;
	self->unaligned_fd = -1;
	self->fd = p3s_open_fixture("/dev/vm_iomap_memory_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "vm_iomap_memory fixture is unavailable");
	ASSERT_GE(self->fd, 0);
	self->unaligned_fd = p3s_open_fixture("/dev/vm_iomap_memory_p3s",
					      O_RDWR);
	ASSERT_GE(self->unaligned_fd, 0);
	ASSERT_EQ(0, ioctl(self->unaligned_fd,
			   VM_IOMAP_MEMORY_P3S_SET_UNALIGNED));
	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
}

FIXTURE_TEARDOWN(iomap)
{
	if (self->unaligned_fd >= 0)
		close(self->unaligned_fd);
	if (self->fd >= 0)
		close(self->fd);
}

static unsigned char *map_guarded_shared(int fd, void **reservation)
{
	unsigned char *mapping;

	*reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return MAP_FAILED;
	mapping = mmap(*reservation, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping != *reservation) {
		munmap(*reservation, NATIVE_PAGE_SIZE);
		return MAP_FAILED;
	}
	return mapping;
}

static unsigned char *map_guarded_private(int fd, void **reservation)
{
	uintptr_t target;
	unsigned char *mapping;

	*reservation = mmap(NULL, PRIVATE_RESERVATION_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return MAP_FAILED;
	target = ((uintptr_t)*reservation + NATIVE_PAGE_SIZE - 1) &
		 ~(NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)target, PROCESS_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
	if (mapping != (void *)target) {
		munmap(*reservation, PRIVATE_RESERVATION_SIZE);
		return MAP_FAILED;
	}
	return mapping;
}

TEST_F(iomap, compat_first_slice_is_readable)
{
	unsigned char *mapping, value = 0;
	void *reservation;

	mapping = map_guarded_shared(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(read_byte(mapping, &value));
	EXPECT_EQ(FIRST_MARKER, value);
	munmap(reservation, NATIVE_PAGE_SIZE);
}

TEST_F(iomap, compat_guard_slices_are_unmapped)
{
	unsigned char *mapping, value;
	void *reservation;
	unsigned int guard;

	mapping = map_guarded_shared(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	for (guard = 1; guard < P3S_SLICES; guard++)
		EXPECT_FALSE(read_byte(mapping + guard * PROCESS_PAGE_SIZE,
					 &value));
	munmap(reservation, NATIVE_PAGE_SIZE);
}

TEST_F(iomap, private_pfn_mapping_has_device_data)
{
	unsigned char *mapping;
	void *reservation;

	mapping = map_guarded_private(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(mapping_has_values(mapping, PROCESS_PAGE_SIZE));
	munmap(reservation, PRIVATE_RESERVATION_SIZE);
}

TEST_F(iomap, private_pfn_cow_preserves_full_process_page)
{
	unsigned char *mapping;
	void *reservation;

	mapping = map_guarded_private(self->fd, &reservation);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(mapping_has_values(mapping, PROCESS_PAGE_SIZE));
	mapping[COW_OFFSET] = COW_MARKER;
	EXPECT_TRUE(mapping_has_values(mapping, COW_OFFSET));
	munmap(reservation, PRIVATE_RESERVATION_SIZE);
}

TEST_F(iomap, compat_rejects_non_native_aligned_offset)
{
	unsigned char *reservation, *mapping;
	int error;

	reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	mapping = mmap(reservation, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, self->fd, PROCESS_PAGE_SIZE);
	error = errno;
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	EXPECT_EQ(mapping, MAP_FAILED);
	EXPECT_EQ(EINVAL, error);
	munmap(reservation, NATIVE_PAGE_SIZE);
}

TEST_F(iomap, compat_rejects_mapping_at_buffer_end)
{
	void *mapping;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, NATIVE_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	EXPECT_EQ(mapping, MAP_FAILED);
}

TEST_F(iomap, compat_rejects_unaligned_resource)
{
	void *mapping;
	int error;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->unaligned_fd, 0);
	error = errno;
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	EXPECT_EQ(mapping, MAP_FAILED);
	EXPECT_EQ(EOPNOTSUPP, error);
}

TEST_F(iomap, compat_rejects_unaligned_resource_end)
{
	void *mapping;
	int error;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->unaligned_fd, PROCESS_PAGE_SIZE);
	error = errno;
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	EXPECT_EQ(mapping, MAP_FAILED);
	EXPECT_EQ(EOPNOTSUPP, error);
}

P3S_HARNESS_4KB_MAIN
