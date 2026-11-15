/* SPDX-License-Identifier: GPL-2.0 */
/* Helpers for the four process-page slices of one native kernel page. */
#ifndef P3S_SLICE_TEST_H
#define P3S_SLICE_TEST_H

#include <sys/mman.h>

#include "p3s_test.h"


/* /proc/self/pagemap */

/* PFNs of four arbitrary present pages; pfn may be 0 without CAP_SYS_ADMIN. */
static inline bool read_pagemap_pfns(const unsigned char *const address[P3S_SLICES],
				     uint64_t pfn[P3S_SLICES])
{
	uint64_t entry;
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++) {
		if (!p3s_pagemap_entry(address[i], &entry) ||
		    !(entry & PM_PRESENT))
			return false;
		pfn[i] = p3s_native_pfn(PAGEMAP_PFN(entry));
	}
	return true;
}

/* PFNs of the four slices of the native page at @base. */
static inline bool read_pfns(const unsigned char *base, uint64_t pfn[P3S_SLICES])
{
	const unsigned char *address[P3S_SLICES];
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		address[i] = base + i * PROCESS_PAGE_SIZE;
	return read_pagemap_pfns(address, pfn);
}

/* All four slices map the same (visible) native page. */
static inline bool same_pfn(const uint64_t pfn[P3S_SLICES])
{
	unsigned int i;

	if (!pfn[0])
		return false;
	for (i = 1; i < P3S_SLICES; i++)
		if (pfn[i] != pfn[0])
			return false;
	return true;
}

/*
 * Map exactly @size accessible bytes on a native-page boundary with PROT_NONE
 * guards on both sides, so the VMA starts and ends where the test says it
 * does.  The reservation to unmap spans size + 2 * NATIVE_PAGE_SIZE.
 */
static inline unsigned char *map_exact_edge(size_t size,
					    unsigned char **reservation)
{
	unsigned char *mapping, *base;
	size_t length = size + 2 * NATIVE_PAGE_SIZE;

	mapping = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	base = (unsigned char *)(((uintptr_t)mapping + 2 * NATIVE_PAGE_SIZE - 1) &
				 ~(NATIVE_PAGE_SIZE - 1));
	if (mprotect(base, size, PROT_READ | PROT_WRITE)) {
		munmap(mapping, length);
		return MAP_FAILED;
	}
	*reservation = mapping;
	return base;
}

/* Page slices contents: slice i is filled with the byte @first + i. */

static inline void fill_slices(unsigned char *base, unsigned char first)
{
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		memset(base + i * PROCESS_PAGE_SIZE, first + i, PROCESS_PAGE_SIZE);
}

static inline bool check_slices(const unsigned char *base, unsigned char first)
{
	unsigned long offset;

	for (offset = 0; offset < NATIVE_PAGE_SIZE; offset++)
		if (base[offset] != first + offset / PROCESS_PAGE_SIZE)
			return false;
	return true;
}

static inline bool all_bytes_are(const unsigned char *p, unsigned long len,
				 unsigned char want)
{
	unsigned long i;

	for (i = 0; i < len; i++)
		if (p[i] != want)
			return false;
	return true;
}

#endif /* P3S_SLICE_TEST_H */
