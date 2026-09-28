/* SPDX-License-Identifier: GPL-2.0 */
/* Helpers for the four process-page slices of one native kernel page. */
#ifndef P3S_SLICE_TEST_H
#define P3S_SLICE_TEST_H

#include <sys/mman.h>

#include "p3s_test.h"

/* MADV_PAGEOUT is asynchronous; poll smaps this often, 10ms apart. */
#define PAGEOUT_POLLS		1000

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

/* The listed slices are present and share one native page. */
static inline bool same_present_pfns(const unsigned char *base,
				     const unsigned int *slices,
				     unsigned int nr)
{
	uint64_t first = 0, pfn;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (!p3s_pfn(base + slices[i] * PROCESS_PAGE_SIZE, &pfn))
			return false;
		if (i && pfn != first)
			return false;
		first = pfn;
	}
	return true;
}

/* Exactly one slice of the native page at @base is present. */
static inline bool only_slice_present(const unsigned char *base,
				      unsigned int present_slice)
{
	uint64_t entry;
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++) {
		if (!p3s_pagemap_entry(base + i * PROCESS_PAGE_SIZE, &entry) ||
		    !!(entry & PM_PRESENT) != (i == present_slice))
			return false;
	}
	return true;
}

/* /proc/self/status */

/* RssAnon of this process in bytes, or -1. */
static inline long rss_anon_bytes(void)
{
	long kb = p3s_status_kb("RssAnon");

	return kb < 0 ? -1 : kb * 1024;
}

/* Wait for an already requested pageout to move @minimum bytes to swap. */
static inline bool wait_for_swap(void *address, unsigned long length,
				 unsigned long minimum)
{
	unsigned long swap;
	unsigned int attempt;

	for (attempt = 0; attempt < PAGEOUT_POLLS; attempt++) {
		if (!p3s_smaps_bytes(address, length, "Swap", &swap)) {
			ksft_print_msg("Swap accounting unavailable: length=%lu\n", length);
			return false;
		}
		if (swap >= minimum)
			return true;
		usleep(10000);
	}
	ksft_print_msg("pageout stopped at Swap=%lu, expected at least %lu bytes\n",
		       swap, minimum);
	return false;
}

/* Request pageout and wait until the specified range is really swapped. */
static inline bool pageout_reaches_swap(void *address, unsigned long length,
					unsigned long minimum)
{
	if (madvise(address, length, MADV_PAGEOUT)) {
		ksft_print_msg("MADV_PAGEOUT failed: errno=%d length=%lu\n",
			       errno, length);
		return false;
	}
	return wait_for_swap(address, length, minimum);
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
