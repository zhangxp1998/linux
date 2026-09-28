/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __EXEC_P3S_HELPERS_H
#define __EXEC_P3S_HELPERS_H

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

#endif /* __EXEC_P3S_HELPERS_H */
