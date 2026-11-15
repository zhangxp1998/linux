// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process maps a PCI IORESOURCE_MEM BAR through sysfs resourceN in
 * process-page units.  A sub-native-page offset is rejected, rather than
 * remapping the wrong part of a BAR.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <limits.h>
#include <sys/mman.h>

#include "p3s_harness.h"

#define IORESOURCE_MEM	0x00000200ULL

static int find_mappable_bar(size_t *bar_size)
{
	const char *devices_path = "/sys/bus/pci/devices";
	struct dirent *entry;
	DIR *devices;

	devices = opendir(devices_path);
	if (!devices)
		return -1;
	while ((entry = readdir(devices))) {
		unsigned long long start, end, flags;
		char path[PATH_MAX];
		unsigned int bar;
		FILE *file;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s/resource",
			 devices_path, entry->d_name);
		file = fopen(path, "re");
		if (!file)
			continue;
		for (bar = 0;
		     bar < 6 &&
		     fscanf(file, "%llx %llx %llx", &start, &end, &flags) == 3;
		     bar++) {
			unsigned long long len;
			size_t size;
			void *probe;
			int fd;

			if (!(flags & IORESOURCE_MEM) || end <= start)
				continue;
			len = end - start + 1;
			if (len > SIZE_MAX || len <= PROCESS_PAGE_SIZE ||
			    (len % PROCESS_PAGE_SIZE))
				continue;
			size = (size_t)len;
			snprintf(path, sizeof(path), "%s/%s/resource%u",
				 devices_path, entry->d_name, bar);
			fd = open(path, O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				continue;
			probe = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
			if (probe != MAP_FAILED) {
				munmap(probe, size);
				fclose(file);
				closedir(devices);
				*bar_size = size;
				return fd;
			}
			close(fd);
		}
		fclose(file);
	}
	closedir(devices);
	return -1;
}

FIXTURE(pci_mmap) {
	size_t size;
	int fd;
};

FIXTURE_SETUP(pci_mmap)
{
	self->fd = find_mappable_bar(&self->size);
	if (self->fd < 0)
		SKIP(return, "no mappable PCI memory BAR available");
}

FIXTURE_TEARDOWN(pci_mmap)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(pci_mmap, complete_bar_maps_in_process_pages)
{
	void *mapping = mmap(NULL, self->size, PROT_READ, MAP_SHARED,
			     self->fd, 0);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(0, munmap(mapping, self->size));
}

TEST_F(pci_mmap, sub_native_bar_offset_is_rejected)
{
	void *mapping;
	int error;

	errno = 0;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, PROCESS_PAGE_SIZE);
	error = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, error);
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
