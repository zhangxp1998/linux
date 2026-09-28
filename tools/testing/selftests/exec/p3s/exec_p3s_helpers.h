/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __EXEC_P3S_HELPERS_H
#define __EXEC_P3S_HELPERS_H

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline bool p3s_exec_sibling(char *path, size_t size, const char *name)
{
	char *filename;
	ssize_t length = readlink("/proc/self/exe", path, size - 1);
	size_t prefix;

	if (length < 0 || (size_t)length >= size - 1)
		return false;
	path[length] = '\0';
	filename = strrchr(path, '/');
	if (!filename)
		return false;
	prefix = filename + 1 - path;
	if (prefix + strlen(name) + 1 > size)
		return false;
	memcpy(path + prefix, name, strlen(name) + 1);
	return true;
}

static inline int p3s_exec_sysctl(const char *path)
{
	int value = -1;
	FILE *file = fopen(path, "re");

	if (!file)
		return -1;
	if (fscanf(file, "%d", &value) != 1)
		value = -1;
	fclose(file);
	return value;
}

static inline bool p3s_exec_write_sysctl(const char *path, int value)
{
	char text[16];
	int fd, length;
	bool written;

	length = snprintf(text, sizeof(text), "%d\n", value);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	written = write(fd, text, length) == length;
	return close(fd) == 0 && written;
}

#ifdef P3S_ASLR_FIXTURE
#include "p3s_harness.h"

#define P3S_MMAP_RND_BITS_PATH "/proc/sys/vm/mmap_rnd_bits"
#define P3S_MMAP_RND_BITS_CAP 24
#define P3S_MMAP_RND_BITS_TEST 30

FIXTURE(aslr) {
	int original_rnd_bits;
	bool restore;
};

FIXTURE_SETUP(aslr)
{
	int persona = personality(0xffffffffUL);

	if (p3s_exec_sysctl("/proc/sys/kernel/randomize_va_space") <= 0 ||
	    (persona >= 0 && (persona & ADDR_NO_RANDOMIZE)))
		SKIP(return, "address randomization is disabled");
	self->original_rnd_bits = p3s_exec_sysctl(P3S_MMAP_RND_BITS_PATH);
	if (self->original_rnd_bits < 0)
		SKIP(return, "mmap_rnd_bits is unavailable");
	/* Below 26 bits, an uncapped PIE base still fits in the 39-bit space. */
	if (self->original_rnd_bits < P3S_MMAP_RND_BITS_TEST) {
		if (p3s_exec_write_sysctl(P3S_MMAP_RND_BITS_PATH,
					  P3S_MMAP_RND_BITS_TEST))
			self->restore = true;
		else if (self->original_rnd_bits <= P3S_MMAP_RND_BITS_CAP + 1)
			SKIP(return, "cannot raise mmap_rnd_bits to exercise the 4KB cap");
	}
	if (p3s_exec_sysctl(P3S_MMAP_RND_BITS_PATH) <= P3S_MMAP_RND_BITS_CAP + 1) {
		if (self->restore)
			p3s_exec_write_sysctl(P3S_MMAP_RND_BITS_PATH,
					      self->original_rnd_bits);
		SKIP(return, "mmap_rnd_bits cannot exercise the 4KB cap");
	}
}

FIXTURE_TEARDOWN_PARENT(aslr)
{
	if (self->restore)
		EXPECT_TRUE(p3s_exec_write_sysctl(P3S_MMAP_RND_BITS_PATH,
						  self->original_rnd_bits));
}
#endif

#endif /* __EXEC_P3S_HELPERS_H */
