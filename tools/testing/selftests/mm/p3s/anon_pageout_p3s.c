// SPDX-License-Identifier: GPL-2.0
/*
 * MADV_PAGEOUT of a 4K compat process's anonymous pages installs swap PTEs
 * that preserve their contents, and a swapin after a UFFD write-protect mode
 * change drops the stale protection.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_harness.h"
#include "vm_util.h"

#define UFFD_REPRO_MAPPING_SIZE	(16UL * 1024 * 1024)
#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * PROCESS_PAGE_SIZE)
#define POLL_ATTEMPTS	200

static bool swap_info(unsigned long *total_bytes, unsigned long *free_bytes)
{
	unsigned long total_kb = 0;
	unsigned long free_kb = 0;
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	if (!meminfo)
		return false;
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (sscanf(line, "SwapTotal: %lu kB", &total_kb) == 1)
			continue;
		if (sscanf(line, "SwapFree: %lu kB", &free_kb) == 1)
			continue;
	}
	free(line);
	fclose(meminfo);
	*total_bytes = total_kb * 1024;
	*free_bytes = free_kb * 1024;
	return true;
}

static bool vma_usage_bytes(const void *address, unsigned long *rss_bytes,
			    unsigned long *swap_bytes)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end, value_kb;
	char *line = NULL;
	size_t capacity = 0;
	bool found_rss = false;
	bool found_swap = false;
	bool in_target = false;
	FILE *smaps;

	smaps = fopen("/proc/self/smaps", "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_target)
				break;
			in_target = target >= start && target < end;
			continue;
		}
		if (!in_target)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found_rss = true;
		} else if (sscanf(line, "Swap: %lu kB", &value_kb) == 1) {
			*swap_bytes = value_kb * 1024;
			found_swap = true;
		}
	}
	free(line);
	fclose(smaps);
	return found_rss && found_swap;
}

static bool vma_span(const void *address, unsigned long *span)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return false;
	while (getline(&line, &capacity, maps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;
		if (target >= start && target < end) {
			*span = end - start;
			found = true;
			break;
		}
	}
	free(line);
	fclose(maps);
	return found;
}

static bool page_out_mapping(void *mapping, unsigned long *rss_bytes,
			     unsigned long *swap_bytes,
			     unsigned long *device_bytes)
{
	unsigned long total;
	unsigned long free_before;
	unsigned long free_after;
	unsigned int attempt;

	if (!swap_info(&total, &free_before))
		return false;
	if (madvise(mapping, MAPPING_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!swap_info(&total, &free_after) ||
		    !vma_usage_bytes(mapping, rss_bytes, swap_bytes))
			return false;
		*device_bytes = free_after < free_before ? free_before - free_after : 0;
		if (*swap_bytes >= MAPPING_SIZE / 2 &&
		    *rss_bytes <= MAPPING_SIZE / 2)
			return true;
		usleep(10000);
	}
	return false;
}

static int swapin_after_uffd_wp_mode_change(void)
{
	struct uffdio_writeprotect writeprotect = {
		.range.len = PROCESS_PAGE_SIZE,
		.mode = UFFDIO_WRITEPROTECT_MODE_WP,
	};
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP,
	};
	unsigned long rss = 0;
	unsigned long swap = 0;
	size_t mapping_size = UFFD_REPRO_MAPPING_SIZE + NATIVE_PAGE_SIZE;
	unsigned char *mapping = MAP_FAILED;
	unsigned char *base;
	unsigned int attempt;
	int passed = 0;
	const char *failure = "mmap";
	int uffd = -1;

	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		goto out;
	base = (unsigned char *)(((unsigned long)mapping +
						 NATIVE_PAGE_SIZE - 1) &
					~(NATIVE_PAGE_SIZE - 1)) +
		UFFD_REPRO_MAPPING_SIZE - NATIVE_PAGE_SIZE;

	base[0] = 0x40;
	base[PROCESS_PAGE_SIZE] = 0x41;
	base[2 * PROCESS_PAGE_SIZE] = 0x42;
	base[3 * PROCESS_PAGE_SIZE] = 0x43;

	failure = "madvise";
	if (madvise(base, NATIVE_PAGE_SIZE, MADV_PAGEOUT))
		goto out;
	failure = "swapout";
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!vma_usage_bytes(base + PROCESS_PAGE_SIZE, &rss, &swap))
			goto out;
		if (swap >= NATIVE_PAGE_SIZE && !rss)
			break;
		usleep(10000);
	}
	if (attempt == POLL_ATTEMPTS)
		goto out;

	failure = "UFFDIO_API";
	uffd = syscall(SYS_userfaultfd, O_NONBLOCK | UFFD_USER_MODE_ONLY);
	if (uffd < 0 || ioctl(uffd, UFFDIO_API, &api))
		goto out;
	failure = "UFFDIO_REGISTER_MODE_WP";
	if (uffd_register(uffd, base + PROCESS_PAGE_SIZE,
			  2 * PROCESS_PAGE_SIZE, false, true, false))
		goto out;
	writeprotect.range.start = (unsigned long)base + PROCESS_PAGE_SIZE;
	failure = "UFFDIO_WRITEPROTECT";
	if (ioctl(uffd, UFFDIO_WRITEPROTECT, &writeprotect))
		goto out;

	failure = "UFFDIO_REGISTER_MODE_MISSING/mlock";
	if (uffd_register(uffd, base, NATIVE_PAGE_SIZE, true, false, false) ||
	    mlock(base + PROCESS_PAGE_SIZE, 3 * PROCESS_PAGE_SIZE))
		goto out;
	munlock(base + PROCESS_PAGE_SIZE, 3 * PROCESS_PAGE_SIZE);
	passed = base[PROCESS_PAGE_SIZE] == 0x41 &&
		 base[2 * PROCESS_PAGE_SIZE] == 0x42 &&
		 base[3 * PROCESS_PAGE_SIZE] == 0x43;
	if (passed) {
		base[PROCESS_PAGE_SIZE] = 0xa5;
		passed = base[PROCESS_PAGE_SIZE] == 0xa5;
	}
out:
	if (!passed && !strcmp(failure, "swapout") && !swap) {
		ksft_print_msg("MADV_PAGEOUT did not evict the test folio\n");
		passed = -1;
	}
	if (!passed)
		ksft_print_msg("UFFD mode-change setup failed at %s: %s (Rss: %lu, Swap: %lu)\n",
			       failure, strerror(errno), rss, swap);
	if (uffd >= 0)
		close(uffd);
	if (mapping != MAP_FAILED)
		munmap(mapping, mapping_size);
	return passed;
}

static int swap_state(unsigned long *free_swap)
{
	unsigned long total_swap = 0;

	if (!swap_info(&total_swap, free_swap))
		return -1;
	return total_swap ? 1 : 0;
}

TEST(swap_has_free_space)
{
	unsigned long free_swap = 0;
	int state = swap_state(&free_swap);

	if (!state)
		SKIP(return, "no active swap device");
	ASSERT_EQ(1, state);
	EXPECT_GT(free_swap, 0);
}

TEST(swapin_drops_stale_uffd_write_protection)
{
	unsigned long free_swap = 0;
	int result, state = swap_state(&free_swap);

	if (!state)
		SKIP(return, "no active swap device");
	ASSERT_EQ(1, state);
	result = swapin_after_uffd_wp_mode_change();
	if (result < 0)
		SKIP(return, "MADV_PAGEOUT did not evict the test folio");
	EXPECT_EQ(1, result);
}

FIXTURE(pageout) {
	unsigned char *reservation;
	unsigned char *mapping;
	unsigned long span;
	unsigned long resident;
	unsigned long swapped;
	unsigned long device_bytes;
	bool span_ok;
	bool paged_out;
};

FIXTURE_SETUP(pageout)
{
	unsigned long free_swap = 0;
	unsigned int i;
	int state = swap_state(&free_swap);

	if (!state)
		SKIP(return, "no active swap device");
	ASSERT_EQ(1, state);
	ASSERT_GT(free_swap, 0);
	self->reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	self->mapping = mmap(self->reservation + PROCESS_PAGE_SIZE,
			     MAPPING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_NE(self->mapping, MAP_FAILED);
	madvise(self->mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
	for (i = 0; i < TEST_PAGES; i++)
		self->mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i;
	self->span_ok = vma_span(self->mapping, &self->span);
	self->paged_out = page_out_mapping(self->mapping, &self->resident,
					    &self->swapped,
					    &self->device_bytes);
}

FIXTURE_TEARDOWN(pageout)
{
	munmap(self->reservation, RESERVE_SIZE);
}

TEST_F(pageout, target_is_isolated_process_page_vma)
{
	ASSERT_TRUE(self->span_ok);
	EXPECT_EQ(MAPPING_SIZE, self->span);
}

TEST_F(pageout, pageout_installs_swap_ptes)
{
	TH_LOG("Swap: %lu, Rss: %lu, device: %lu bytes",
	       self->swapped, self->resident, self->device_bytes);
	EXPECT_TRUE(self->paged_out);
}

TEST_F(pageout, swapped_process_pages_preserve_contents)
{
	unsigned int i;

	ASSERT_TRUE(self->paged_out);
	for (i = 0; i < TEST_PAGES; i++)
		EXPECT_EQ((unsigned char)(0x40 + i),
			  self->mapping[i * PROCESS_PAGE_SIZE]);
}

P3S_HARNESS_COMPAT_MAIN
