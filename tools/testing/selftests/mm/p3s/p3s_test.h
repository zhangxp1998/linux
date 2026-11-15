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

static inline bool p3s_page_swapped(const void *address)
{
	uint64_t entry;

	return p3s_pagemap_entry(address, &entry) &&
	       !(entry & PM_PRESENT) && (entry & PM_SWAP);
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
