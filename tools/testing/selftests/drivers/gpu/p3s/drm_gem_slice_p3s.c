// SPDX-License-Identifier: GPL-2.0
/* DRM GEM mmap rejects nonzero 4KB slice offsets into a dumb buffer cookie. */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"

FIXTURE(drm_gem) {
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	struct drm_mode_destroy_dumb destroy;
	int fd;
};

FIXTURE_SETUP(drm_gem)
{
	char path[64];
	unsigned int i;

	ASSERT_TRUE(getpagesize() == PROCESS_PAGE_SIZE ||
		    getpagesize() == NATIVE_PAGE_SIZE);
	self->create = (struct drm_mode_create_dumb) {
		.width = 64, .height = 64, .bpp = 32,
	};
	self->fd = -1;
	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%u", i);
		self->fd = open(path, O_RDWR | O_CLOEXEC);
		if (self->fd < 0)
			continue;
		if (!ioctl(self->fd, DRM_IOCTL_MODE_CREATE_DUMB,
			   &self->create))
			break;
		close(self->fd);
		self->fd = -1;
	}
	if (self->fd < 0)
		SKIP(return, "no DRM device supports a test-owned dumb buffer");
	self->map.handle = self->create.handle;
	self->destroy.handle = self->create.handle;
	if (ioctl(self->fd, DRM_IOCTL_MODE_MAP_DUMB, &self->map)) {
		ioctl(self->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
		      &self->destroy);
		close(self->fd);
		SKIP(return, "DRM device cannot map its dumb buffer");
	}
}

FIXTURE_TEARDOWN(drm_gem)
{
	EXPECT_EQ(0, ioctl(self->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
			    &self->destroy));
	close(self->fd);
}

TEST_F(drm_gem, aligned_gem_cookie_preserves_buffer_edges)
{
	unsigned int *first, *last;
	void *mapping;

	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, self->fd, self->map.offset);
	ASSERT_NE(mapping, MAP_FAILED);
	first = mapping;
	last = (unsigned int *)((char *)mapping + NATIVE_PAGE_SIZE -
				 sizeof(*last));
	__atomic_store_n(first, 0x6a39421b, __ATOMIC_RELAXED);
	__atomic_store_n(last, 0x5b2861ac, __ATOMIC_RELAXED);
	EXPECT_EQ(0x6a39421b, __atomic_load_n(first, __ATOMIC_RELAXED));
	EXPECT_EQ(0x5b2861ac, __atomic_load_n(last, __ATOMIC_RELAXED));
	munmap(mapping, NATIVE_PAGE_SIZE);
}

static void check_cookie_slice(struct __test_metadata *_metadata,
			       FIXTURE_DATA(drm_gem) *self, unsigned int slice)
{
	void *mapping;
	int saved_errno;

	if (getpagesize() != PROCESS_PAGE_SIZE)
		SKIP(return, "GEM cookie slice requires a 4KB process");
	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, self->fd,
		       self->map.offset + slice * PROCESS_PAGE_SIZE);
	saved_errno = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

TEST_F(drm_gem, reject_cookie_slice_one)
{
	check_cookie_slice(_metadata, self, 1);
}

TEST_F(drm_gem, reject_cookie_slice_two)
{
	check_cookie_slice(_metadata, self, 2);
}

TEST_F(drm_gem, reject_cookie_slice_three)
{
	check_cookie_slice(_metadata, self, 3);
}

P3S_HARNESS_4KB_MAIN
