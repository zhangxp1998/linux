// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define PROCESS_PAGE_SIZE 4096UL
#define FILE_SIZE (16 * PROCESS_PAGE_SIZE)

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const unsigned char *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static int read_valid_data_length(const char *device, uint64_t *valid_size)
{
	unsigned char boot[512];
	unsigned char *root = NULL;
	uint64_t root_offset;
	uint32_t cluster_heap, root_cluster;
	unsigned int sector_shift, cluster_shift;
	size_t cluster_size;
	ssize_t nread;
	int fd = -1;
	int ret = -1;
	size_t off;

	fd = open(device, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto out;
	nread = pread(fd, boot, sizeof(boot), 0);
	if (nread != sizeof(boot))
		goto out;
	if (memcmp(boot + 3, "EXFAT   ", 8)) {
		errno = EINVAL;
		goto out;
	}

	cluster_heap = get_le32(boot + 0x58);
	root_cluster = get_le32(boot + 0x60);
	sector_shift = boot[0x6c];
	cluster_shift = boot[0x6d];
	if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 16 ||
	    root_cluster < 2) {
		errno = EINVAL;
		goto out;
	}
	cluster_size = (size_t)1 << (sector_shift + cluster_shift);
	root_offset = ((uint64_t)cluster_heap +
		       ((uint64_t)root_cluster - 2) * ((uint64_t)1 << cluster_shift))
		      << sector_shift;
	root = malloc(cluster_size);
	if (!root)
		goto out;
	nread = pread(fd, root, cluster_size, root_offset);
	if (nread != (ssize_t)cluster_size)
		goto out;

	for (off = 0; off + 64 <= cluster_size; off += 32) {
		unsigned char secondary_count;

		if (root[off] == 0)
			break;
		if (root[off] != 0x85)
			continue;
		secondary_count = root[off + 1];
		if (!secondary_count || off + 64 > cluster_size ||
		    root[off + 32] != 0xc0)
			continue;
		*valid_size = get_le64(root + off + 32 + 8);
		ret = 0;
		break;
	}
	if (ret && !errno)
		errno = ENOENT;
out:
	free(root);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int run_test(const char *device, const char *mountpoint)
{
	char path[256];
	unsigned char *mapping = MAP_FAILED;
	uint64_t valid_size = 0;
	int fd = -1;
	int ret = EXIT_FAILURE;

	printf("TAP version 13\n1..4\n");
	if (sysconf(_SC_PAGESIZE) != PROCESS_PAGE_SIZE) {
		printf("not ok 1 - process uses 4K pages\n");
		goto out;
	}
	printf("ok 1 - process uses 4K pages\n");

	if (mount(device, mountpoint, "exfat", 0, NULL)) {
		printf("not ok 2 - mount exFAT image: %s\n", strerror(errno));
		goto out;
	}
	printf("ok 2 - mount exFAT image\n");

	if (snprintf(path, sizeof(path), "%s/ppps-vdl.bin", mountpoint) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		goto mounted;
	}
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || ftruncate(fd, FILE_SIZE))
		goto mounted;
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		goto mounted;
	mapping[0] = 0x5a;
	if (msync(mapping, PROCESS_PAGE_SIZE, MS_SYNC) || fsync(fd) || syncfs(fd))
		goto mounted;
	printf("ok 3 - write one process page through shared mmap\n");

	munmap(mapping, PROCESS_PAGE_SIZE);
	mapping = MAP_FAILED;
	close(fd);
	fd = -1;
	if (umount(mountpoint))
		goto out;
	if (read_valid_data_length(device, &valid_size)) {
		printf("not ok 4 - read exFAT valid data length: %s\n",
		       strerror(errno));
		goto out;
	}
	if (valid_size != PROCESS_PAGE_SIZE) {
		printf("not ok 4 - mmap advances valid data by one process page # got %llu\n",
		       (unsigned long long)valid_size);
		goto out;
	}
	printf("ok 4 - mmap advances valid data by one process page\n");
	ret = EXIT_SUCCESS;
	goto out;

mounted:
	printf("not ok 3 - write one process page through shared mmap: %s\n",
	       strerror(errno));
	if (mapping != MAP_FAILED)
		munmap(mapping, PROCESS_PAGE_SIZE);
	if (fd >= 0)
		close(fd);
	umount(mountpoint);
out:
	return ret;
}

int main(int argc, char **argv)
{
	int persona;

	if (argc == 4 && !strcmp(argv[1], "--run"))
		return run_test(argv[2], argv[3]);
	if (argc != 3)
		return EXIT_FAILURE;

	persona = personality(0xffffffffUL);
	if (persona < 0 ||
	    personality((unsigned long)persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0) {
		perror("personality");
		return EXIT_FAILURE;
	}
	execl("/proc/self/exe", "mmap-valid-size-ppps", "--run", argv[1],
	      argv[2], NULL);
	perror("exec");
	return EXIT_FAILURE;
}
