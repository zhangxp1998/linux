// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process maps the QEMU EDU PCI BAR through sysfs resource0 in
 * process-page units.  A sub-native-page offset is rejected, rather than
 * remapping the wrong part of a BAR.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <limits.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8

static int read_hex_value(const char *path, unsigned int *value)
{
	FILE *file = fopen(path, "re");
	int matched;

	if (!file)
		return -errno;
	matched = fscanf(file, "%x", value);
	fclose(file);
	return matched == 1 ? 0 : -EINVAL;
}

static int find_edu_resource(char resource_path[PATH_MAX])
{
	const char *devices_path = "/sys/bus/pci/devices";
	char value_path[PATH_MAX];
	struct dirent *entry;
	unsigned int device;
	unsigned int vendor;
	DIR *devices;

	devices = opendir(devices_path);
	if (!devices)
		return -errno;
	while ((entry = readdir(devices))) {
		if (entry->d_name[0] == '.')
			continue;
		snprintf(value_path, sizeof(value_path), "%s/%s/vendor",
			 devices_path, entry->d_name);
		if (read_hex_value(value_path, &vendor))
			continue;
		snprintf(value_path, sizeof(value_path), "%s/%s/device",
			 devices_path, entry->d_name);
		if (read_hex_value(value_path, &device))
			continue;
		if (vendor != EDU_VENDOR || device != EDU_DEVICE)
			continue;
		snprintf(resource_path, PATH_MAX, "%s/%s/resource0",
			 devices_path, entry->d_name);
		closedir(devices);
		return 0;
	}
	closedir(devices);
	return -ENOENT;
}

static int resource_size(const char *resource_path, size_t *size)
{
	unsigned long long flags;
	unsigned long long start;
	unsigned long long end;
	char path[PATH_MAX];
	char *slash;
	FILE *file;

	if (snprintf(path, sizeof(path), "%s", resource_path) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	slash = strrchr(path, '/');
	if (!slash)
		return -EINVAL;
	strcpy(slash + 1, "resource");
	file = fopen(path, "re");
	if (!file)
		return -errno;
	if (fscanf(file, "%llx %llx %llx", &start, &end, &flags) != 3) {
		fclose(file);
		return -EINVAL;
	}
	fclose(file);
	if (end < start || end - start + 1 > SIZE_MAX)
		return -EOVERFLOW;
	*size = end - start + 1;
	return 0;
}

FIXTURE(pci_edu) {
	char resource_path[PATH_MAX];
	size_t size;
	int fd;
};

FIXTURE_SETUP(pci_edu)
{
	int error;

	error = find_edu_resource(self->resource_path);
	if (error == -ENOENT)
		SKIP(return, "QEMU EDU PCI device is unavailable");
	ASSERT_EQ(0, error);
	ASSERT_EQ(0, resource_size(self->resource_path, &self->size));
	ASSERT_GT(self->size, PROCESS_PAGE_SIZE);
	ASSERT_EQ(0, self->size % PROCESS_PAGE_SIZE);
	self->fd = open(self->resource_path, O_RDWR | O_SYNC);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "EDU BAR resource is unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(pci_edu)
{
	close(self->fd);
}

TEST_F(pci_edu, complete_bar_maps_in_process_pages)
{
	void *mapping = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, self->fd, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(0, munmap(mapping, self->size));
}

TEST_F(pci_edu, sub_native_bar_offset_is_rejected)
{
	void *mapping;
	int error;

	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, self->fd, PROCESS_PAGE_SIZE);
	error = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, error);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_COMPAT_MAIN
