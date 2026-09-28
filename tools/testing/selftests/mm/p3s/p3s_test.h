/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P3S_TEST_H
#define _P3S_TEST_H

#include <limits.h>
#include <sys/mman.h>

#include "kselftest_p3s.h"
#include "vm_util.h"

/* Count VMAs overlapping a range and, optionally, get the offset at start. */
static inline bool p3s_maps_range(const void *address, size_t length,
				  unsigned int *count, unsigned long *offset)
{
	unsigned long low = (unsigned long)address, high = low + length;
	struct procmap_fd procmap;
	unsigned long cursor = low;
	bool found = true;

	if (high < low || open_self_procmap(&procmap))
		return false;
	if (count)
		*count = 0;
	if (offset)
		*offset = ULONG_MAX;
	while (cursor < high) {
		if (!find_vma_procmap(&procmap, (void *)cursor) ||
		    procmap.query.vma_end <= cursor) {
			found = false;
			break;
		}
		if (offset && *offset == ULONG_MAX)
			*offset = procmap.query.vma_offset +
				  low - procmap.query.vma_start;
		if (count)
			++*count;
		cursor = procmap.query.vma_end;
	}
	close_procmap(&procmap);
	return found;
}

static inline int p3s_maps_count(const void *address, size_t length)
{
	unsigned int count;

	return p3s_maps_range(address, length, &count, NULL) ? count : -1;
}

static inline unsigned long p3s_maps_offset(const void *address)
{
	unsigned long offset;

	return p3s_maps_range(address, 1, NULL, &offset) ? offset : ULONG_MAX;
}

/* The pagemap entry of @address in this process. */
static inline bool p3s_pagemap_entry(const void *address, uint64_t *entry)
{
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = pagemap_read_entry(fd, (uintptr_t)address, getpagesize(), entry);
	close(fd);
	return ok;
}

/* P3S pagemap reports PFNs in 4KB units; compare native folios. */
static inline uint64_t p3s_native_pfn(uint64_t pfn)
{
	return getpagesize() == PROCESS_PAGE_SIZE ? pfn / P3S_SLICES : pfn;
}

/* The native PFN of a present page; false when absent or hidden. */
static inline bool p3s_pfn(const void *address, uint64_t *pfn)
{
	uint64_t entry;

	if (!p3s_pagemap_entry(address, &entry) || !(entry & PM_PRESENT))
		return false;
	*pfn = p3s_native_pfn(PAGEMAP_PFN(entry));
	return *pfn != 0;
}

static inline bool p3s_page_present(const void *address)
{
	uint64_t entry;

	return p3s_pagemap_entry(address, &entry) &&
	       (entry & PM_PRESENT);
}

static inline bool p3s_page_swapped(const void *address)
{
	uint64_t entry;

	return p3s_pagemap_entry(address, &entry) &&
	       !(entry & PM_PRESENT) && (entry & PM_SWAP);
}

/*
 * Sum of the "@field:" line, in bytes, over every VMA of this process
 * that overlaps [@address, @address + @length).  False when no VMA matched.
 * With @length 1 this is the field of the single VMA containing @address.
 */
static inline bool p3s_smaps_sum(uintptr_t address, size_t length,
				 const char *field, unsigned long *bytes)
{
	uintptr_t target_end = address + length;
	unsigned long start, end, value_kb;
	char format[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false, in_target = false;
	FILE *smaps;

	*bytes = 0;
	snprintf(format, sizeof(format), "%s: %%lu kB", field);
	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_target = address < end && target_end > start;
			continue;
		}
		if (in_target && sscanf(line, format, &value_kb) == 1) {
			*bytes += value_kb * 1024;
			found = true;
			in_target = false;
		}
	}
	free(line);
	fclose(smaps);
	return found;
}

/* The same for a range of this process. */
static inline bool p3s_smaps_bytes(const void *address, size_t length,
				   const char *field, unsigned long *bytes)
{
	return p3s_smaps_sum((uintptr_t)address, length, field, bytes);
}

/* The "@field:" line in an open status file, in kB, or -1. */
static inline long p3s_read_status_kb(FILE *status, const char *field)
{
	char line[256];
	size_t name_len = strlen(field);
	long value = -1;

	while (fgets(line, sizeof(line), status)) {
		if (!strncmp(line, field, name_len) && line[name_len] == ':') {
			if (sscanf(line + name_len + 1, " %ld kB", &value) != 1)
				value = -1;
			break;
		}
	}
	return value;
}

static inline long p3s_status_kb(const char *field)
{
	FILE *status = fopen("/proc/self/status", "re");
	long value;

	if (!status)
		return -1;
	value = p3s_read_status_kb(status, field);
	fclose(status);
	return value;
}

/* Write an entire string to a control file; preserve the failing errno. */
static inline int p3s_write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int saved;

	if (fd < 0)
		return -1;
	if (!write_full(fd, text, strlen(text))) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return close(fd);
}

static inline bool p3s_swap_available(void)
{
	FILE *file = fopen("/proc/meminfo", "re");
	char line[128];
	unsigned long kb = 0;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file))
		if (sscanf(line, "SwapTotal: %lu kB", &kb) == 1)
			break;
	fclose(file);
	return kb != 0;
}

/* Mappings */

/*
 * Map @size bytes starting on a native-page boundary.  The over-sized
 * reservation to unmap is returned in @reservation; it is size + NATIVE_PAGE_SIZE.
 */
static inline unsigned char *map_aligned_prot(size_t size, int prot,
					      unsigned char **reservation)
{
	unsigned char *mapping;
	uintptr_t aligned;

	mapping = mmap(NULL, size + NATIVE_PAGE_SIZE, prot,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)mapping + NATIVE_PAGE_SIZE - 1) & ~(NATIVE_PAGE_SIZE - 1);
	*reservation = mapping;
	return (unsigned char *)aligned;
}

static inline unsigned char *map_aligned(size_t size,
					 unsigned char **reservation)
{
	return map_aligned_prot(size, PROT_READ | PROT_WRITE, reservation);
}

#endif /* _P3S_TEST_H */
