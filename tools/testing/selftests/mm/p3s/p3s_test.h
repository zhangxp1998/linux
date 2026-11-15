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

#endif /* _P3S_TEST_H */
