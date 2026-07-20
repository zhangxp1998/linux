// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/*
 * A PPPS file folio can be mapped by several 4K PTEs, one per slice.  Those
 * PTEs overlap at struct-page granularity but not necessarily in physical
 * bytes.  Check that smaps applies mapcounts to the matching slice only.
 */

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#define MAPPING_SIZE (4 * PROCESS_PAGE_SIZE)

struct smaps_stats {
	unsigned long rss_kb;
	unsigned long pss_kb;
	unsigned long shared_clean_kb;
	unsigned long shared_dirty_kb;
	unsigned long private_clean_kb;
	unsigned long private_dirty_kb;
};

static bool read_smaps_stats(uintptr_t address, struct smaps_stats *stats)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	memset(stats, 0, sizeof(*stats));
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;

	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, value;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_mapping)
				break;
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value) == 1)
			stats->rss_kb = value;
		else if (sscanf(line, "Pss: %lu kB", &value) == 1)
			stats->pss_kb = value;
		else if (sscanf(line, "Shared_Clean: %lu kB", &value) == 1)
			stats->shared_clean_kb = value;
		else if (sscanf(line, "Shared_Dirty: %lu kB", &value) == 1)
			stats->shared_dirty_kb = value;
		else if (sscanf(line, "Private_Clean: %lu kB", &value) == 1)
			stats->private_clean_kb = value;
		else if (sscanf(line, "Private_Dirty: %lu kB", &value) == 1)
			stats->private_dirty_kb = value;
	}

	free(line);
	fclose(file);
	return in_mapping && stats->rss_kb;
}

static bool stats_equal(const struct smaps_stats *stats,
			unsigned long rss, unsigned long pss,
			unsigned long shared, unsigned long private)
{
	return stats->rss_kb == rss && stats->pss_kb == pss &&
	       stats->shared_clean_kb + stats->shared_dirty_kb == shared &&
	       stats->private_clean_kb + stats->private_dirty_kb == private;
}

static void show_result(const char *name, const struct smaps_stats *stats,
			bool pass, int *test_no, int *failures)
{
	printf("%s %d - %s\n", pass ? "ok" : "not ok", ++*test_no, name);
	printf("# Rss=%lu Pss=%lu Shared=%lu Private=%lu kB\n",
	       stats->rss_kb, stats->pss_kb,
	       stats->shared_clean_kb + stats->shared_dirty_kb,
	       stats->private_clean_kb + stats->private_dirty_kb);
	if (!pass)
		(*failures)++;
}

static int run_test(void)
{
	struct smaps_stats stats;
	unsigned char *alias = MAP_FAILED;
	unsigned char *mapping = MAP_FAILED;
	unsigned long i;
	int failures = 0;
	int test_no = 0;
	int fd = -1;

	printf("TAP version 13\n1..4\n");

	fd = syscall(SYS_memfd_create, "smaps-pss-ppps", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return EXIT_FAILURE;
	}
	if (ftruncate(fd, MAPPING_SIZE)) {
		perror("ftruncate");
		return EXIT_FAILURE;
	}
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	for (i = 0; i < MAPPING_SIZE; i += PROCESS_PAGE_SIZE)
		mapping[i] = (unsigned char)(0x40 + i / PROCESS_PAGE_SIZE);

	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read smaps");
		goto out;
	}
	show_result("disjoint slices are private and have full PSS", &stats,
		    stats_equal(&stats, 16, 16, 0, 16),
		    &test_no, &failures);

	alias = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	if (alias == MAP_FAILED) {
		perror("mmap alias");
		goto out;
	}
	alias[0] ^= 1;
	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read target smaps with alias");
		goto out;
	}
	show_result("one aliased slice only shares its own PSS", &stats,
		    stats_equal(&stats, 16, 14, 4, 12),
		    &test_no, &failures);

	if (!read_smaps_stats((uintptr_t)alias, &stats)) {
		perror("read alias smaps");
		goto out;
	}
	show_result("the alias gets half of its shared slice", &stats,
		    stats_equal(&stats, 4, 2, 4, 0),
		    &test_no, &failures);

	munmap(alias, PROCESS_PAGE_SIZE);
	alias = MAP_FAILED;
	if (!read_smaps_stats((uintptr_t)mapping, &stats)) {
		perror("read target smaps after unmap");
		goto out;
	}
	show_result("unmapping the alias restores exclusive accounting", &stats,
		    stats_equal(&stats, 16, 16, 0, 16),
		    &test_no, &failures);

out:
	if (test_no < 4)
		failures += 4 - test_no;
	if (alias != MAP_FAILED)
		munmap(alias, PROCESS_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);
	if (fd >= 0)
		close(fd);
	printf("# Totals: pass:%d fail:%d\n", 4 - failures, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (access("/proc/self", F_OK) &&
	    mount("proc", "/proc", "proc", 0, NULL)) {
		perror("mount proc");
		return EXIT_FAILURE;
	}
	return ppps_compat_main(argc, argv, run_test);
}
