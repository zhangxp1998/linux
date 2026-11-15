// SPDX-License-Identifier: GPL-2.0
/*
 * Verify swap I/O and byte integrity on a non-synchronous loop device.
 * Native sequential faults prefetch an adjacent native page. P3S restores
 * sibling slices together and keeps the baseline process-PTE history,
 * so the same native-page stride does not request extra readahead.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define ADDRESS_ALIGN	(64 * 1024UL)
#define TEST_PAGES	64
#define TEST_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(TEST_SIZE + 2 * ADDRESS_ALIGN)
#define POLL_ATTEMPTS	300

static const char *page_cluster_path = "/proc/sys/vm/page-cluster";
static const char *vma_ra_path = "/sys/kernel/mm/swap/vma_ra_enabled";

static bool read_named_value(const char *path, const char *name,
			     unsigned long *value)
{
	char key[64];
	unsigned long current;
	char *line = NULL;
	size_t capacity = 0;
	FILE *file;

	file = fopen(path, "re");
	if (!file)
		return false;
	while (getline(&line, &capacity, file) >= 0) {
		if (sscanf(line, "%63s %lu", key, &current) == 2 &&
		    !strcmp(key, name)) {
			*value = current;
			free(line);
			fclose(file);
			return true;
		}
	}
	free(line);
	fclose(file);
	return false;
}

static bool read_ulong_file(const char *path, unsigned long *value)
{
	FILE *file = fopen(path, "re");
	bool success = file && fscanf(file, "%lu", value) == 1;

	if (file)
		fclose(file);
	return success;
}

static bool read_word_file(const char *path, char *word, size_t size)
{
	FILE *file = fopen(path, "re");
	bool success = file && fgets(word, size, file);

	if (file)
		fclose(file);
	if (success)
		word[strcspn(word, "\n")] = '\0';
	return success && word[0];
}

/* A sole loop device prevents an existing zram per-CPU cluster being reused. */
static bool verified_swap_fixture(void)
{
	char line[512], device[256], type[64], path[320];
	unsigned long rotational;
	const char *base;
	FILE *swaps;
	int count = 0;
	bool valid = false;

	swaps = fopen("/proc/swaps", "re");
	if (!swaps)
		return false;
	if (!fgets(line, sizeof(line), swaps))
		goto out;
	while (fgets(line, sizeof(line), swaps)) {
		if (sscanf(line, "%255s %63s", device, type) != 2)
			goto out;
		count++;
		if (count != 1 || strcmp(type, "partition"))
			goto out;
		base = strrchr(device, '/');
		base = base ? base + 1 : device;
		if (strncmp(base, "loop", 4) || !base[4])
			goto out;
		for (const char *digit = base + 4; *digit; digit++) {
			if (*digit < '0' || *digit > '9')
				goto out;
		}
		if (snprintf(path, sizeof(path),
			     "/sys/class/block/%s/queue/rotational", base) >=
		    (int)sizeof(path) ||
		    !read_ulong_file(path, &rotational) || rotational)
			goto out;
	}
	valid = count == 1;
out:
	fclose(swaps);
	return valid;
}

static bool write_text_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "we");
	bool success;

	if (!file)
		return false;
	success = fputs(text, file) >= 0;
	if (fclose(file))
		success = false;
	return success;
}

static bool mapping_usage(const void *address, unsigned long *rss_bytes,
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

static bool mapping_span(const void *address, unsigned long *span)
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
			     unsigned long *swap_bytes)
{
	unsigned int attempt;

	if (madvise(mapping, TEST_SIZE, MADV_PAGEOUT))
		return false;
	for (attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
		if (!mapping_usage(mapping, rss_bytes, swap_bytes))
			return false;
		if (*swap_bytes >= TEST_SIZE / 2 && *rss_bytes <= TEST_SIZE / 2)
			return true;
		usleep(10000);
	}
	return false;
}

/* mincore reports up-to-date swap-cache folios for swapped anonymous PTEs. */
static bool cache_absent(unsigned char *mapping, unsigned int page)
{
	unsigned char resident[P3S_SLICES];
	unsigned int i;

	if (mincore(mapping + page * NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		    resident))
		return false;
	for (i = 0; i < NATIVE_PAGE_SIZE / (unsigned int)getpagesize(); i++) {
		if (resident[i] & 1)
			return false;
	}
	return true;
}

static bool cache_present(unsigned char *mapping, unsigned int page)
{
	unsigned char resident[P3S_SLICES];
	unsigned int i;

	if (mincore(mapping + page * NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		    resident))
		return false;
	for (i = 0; i < NATIVE_PAGE_SIZE / (unsigned int)getpagesize(); i++) {
		if (!(resident[i] & 1))
			return false;
	}
	return true;
}

static bool test_pages_cold(unsigned char *mapping)
{
	return cache_absent(mapping, 0) && cache_absent(mapping, 8) &&
	       cache_absent(mapping, 9) && cache_absent(mapping, 10);
}

/* Apply bounded pressure and stop as soon as the test pages are evicted. */
static bool evict_swap_cache(unsigned char *mapping)
{
	unsigned long available_kb = 0;
	struct timespec started, now;
	size_t budget, offset;
	unsigned char *pressure;
	bool cold;

	if (test_pages_cold(mapping))
		return true;
	if (!read_named_value("/proc/meminfo", "MemAvailable:",
			      &available_kb) || available_kb < 256 * 1024)
		return false;
	budget = available_kb * 1024 / 2;
	if (budget > 768UL * 1024 * 1024)
		budget = 768UL * 1024 * 1024;
	if (clock_gettime(CLOCK_MONOTONIC, &started))
		return false;
	pressure = mmap(NULL, budget, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pressure == MAP_FAILED)
		return false;
	madvise(pressure, budget, MADV_NOHUGEPAGE);
	cold = false;
	for (offset = 0; offset < budget; offset += PROCESS_PAGE_SIZE) {
		pressure[offset] = offset / PROCESS_PAGE_SIZE;
		if ((offset & ((32UL * 1024 * 1024) - 1)) == 0) {
			if (test_pages_cold(mapping)) {
				cold = true;
				break;
			}
			if (clock_gettime(CLOCK_MONOTONIC, &now) ||
			    now.tv_sec - started.tv_sec >= 10)
				break;
		}
	}
	cold = cold || test_pages_cold(mapping);
	munmap(pressure, budget);
	return cold;
}

static long major_faults(void)
{
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage))
		return -1;
	return usage.ru_majflt;
}

static bool fault_and_check(unsigned char *mapping, unsigned int page,
			    long *major_delta)
{
	long before = major_faults();
	unsigned char value = mapping[page * NATIVE_PAGE_SIZE];
	long after = major_faults();

	if (before < 0 || after < 0)
		return false;
	*major_delta = after - before;
	return value == (unsigned char)(0x40 + page);
}


FIXTURE(swap_readahead) {
	unsigned long saved_page_cluster;
	char saved_vma_ra[16];
	bool restore_page_cluster;
	bool restore_vma_ra;
};

FIXTURE_SETUP(swap_readahead)
{
}

/* The test can fail after changing either kernel-wide readahead setting. */
FIXTURE_TEARDOWN_PARENT(swap_readahead)
{
	char value[32];

	if (self->restore_page_cluster) {
		snprintf(value, sizeof(value), "%lu\n",
			 self->saved_page_cluster);
		EXPECT_TRUE(write_text_file(page_cluster_path, value));
	}
	if (self->restore_vma_ra)
		EXPECT_TRUE(write_text_file(vma_ra_path, self->saved_vma_ra));
}

TEST_F(swap_readahead, adjacent_native_fault_uses_process_readahead_policy)
{
	unsigned long total_swap_kb = 0, rss_bytes = 0, swap_bytes = 0;
	unsigned long span = 0, swap_ra_before = 0, swap_ra_after = 0;
	unsigned char *reservation, *mapping;
	uintptr_t aligned;
	long major_delta[3] = {};
	bool mapped;

	if (!verified_swap_fixture())
		SKIP(return, "requires sole non-rotational loop swap fixture");
	ASSERT_TRUE(read_named_value("/proc/meminfo", "SwapTotal:",
				     &total_swap_kb));
	ASSERT_GT(total_swap_kb, 0);
	self->restore_page_cluster = read_ulong_file(page_cluster_path,
						     &self->saved_page_cluster);
	self->restore_vma_ra = read_word_file(vma_ra_path,
						 self->saved_vma_ra,
						 sizeof(self->saved_vma_ra));
	ASSERT_TRUE(self->restore_page_cluster);
	ASSERT_TRUE(self->restore_vma_ra);
	ASSERT_TRUE(write_text_file(page_cluster_path, "1\n"));
	ASSERT_TRUE(write_text_file(vma_ra_path, "true\n"));

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	aligned = ((uintptr_t)reservation + 2 * ADDRESS_ALIGN - 1) &
		  ~(uintptr_t)(ADDRESS_ALIGN - 1);
	mapping = mmap((void *)aligned, TEST_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_EQ((void *)aligned, mapping);
	mapped = !(aligned & (ADDRESS_ALIGN - 1)) &&
		 (!madvise(mapping, TEST_SIZE, MADV_NOHUGEPAGE) ||
		  errno == EINVAL) &&
		 mapping_span(mapping, &span) && span == TEST_SIZE;
	ASSERT_TRUE(mapped);
	for (unsigned int i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i / P3S_SLICES;
	ASSERT_TRUE(page_out_mapping(mapping, &rss_bytes, &swap_bytes));
	TH_LOG("Swap=%lu Rss=%lu bytes", swap_bytes, rss_bytes);
	if (!evict_swap_cache(mapping))
		SKIP(return, "bounded pressure did not evict target and neighbor");
	ASSERT_TRUE(fault_and_check(mapping, 0, &major_delta[0]));
	ASSERT_GT(major_delta[0], 0);
	ASSERT_TRUE(fault_and_check(mapping, 8, &major_delta[1]));
	ASSERT_GT(major_delta[1], 0);
	if (!cache_absent(mapping, 9) || !cache_absent(mapping, 10))
		SKIP(return, "adjacent target or neighbor already cached");
	ASSERT_TRUE(read_named_value("/proc/vmstat", "swap_ra",
				     &swap_ra_before));
	ASSERT_TRUE(fault_and_check(mapping, 9, &major_delta[2]));
	ASSERT_GT(major_delta[2], 0);
	ASSERT_TRUE(read_named_value("/proc/vmstat", "swap_ra",
				     &swap_ra_after));
	TH_LOG("major faults=%ld,%ld,%ld swap_ra=%lu->%lu",
	       major_delta[0], major_delta[1], major_delta[2],
	       swap_ra_before, swap_ra_after);
	if (getpagesize() == PROCESS_PAGE_SIZE) {
		EXPECT_TRUE(cache_absent(mapping, 10));
		EXPECT_EQ(swap_ra_before, swap_ra_after);
	} else {
		EXPECT_TRUE(cache_present(mapping, 10));
		EXPECT_GT(swap_ra_after, swap_ra_before);
	}
	munmap(reservation, RESERVE_SIZE);
}

int main(int argc, char **argv)
{
	return p3s_geometry_main(argc, argv, p3s_harness_run);
}
