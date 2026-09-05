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
#include <sys/wait.h>
#include <poll.h>

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

static bool read_process_smaps(pid_t pid, uintptr_t address,
			       struct smaps_stats *stats)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;
	char path[64];

	memset(stats, 0, sizeof(*stats));
	snprintf(path, sizeof(path), "/proc/%ld/smaps", (long)pid);
	file = fopen(path, "re");
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

static bool read_smaps_stats(uintptr_t address, struct smaps_stats *stats)
{
	return read_process_smaps(getpid(), address, stats);
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
	ksft_test_result(pass, "%s\n", name);
	++*test_no;
	printf("# Rss=%lu Pss=%lu Shared=%lu Private=%lu kB\n",
	       stats->rss_kb, stats->pss_kb,
	       stats->shared_clean_kb + stats->shared_dirty_kb,
	       stats->private_clean_kb + stats->private_dirty_kb);
	if (!pass)
		(*failures)++;
}

/* Native child holds one 16K mapping while the compat parent aliases 4K. */
static int native_pss_child(int fd, int ready, int done)
{
	unsigned char *mapping = MAP_FAILED;
	uintptr_t address = 0;
	char token;
	bool ok;

	if (getpagesize() == NATIVE_PAGE_SIZE) {
		mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
		if (mapping == MAP_FAILED)
			return EXIT_FAILURE;
		mapping[0] = 0x54;
		address = (uintptr_t)mapping;
	}
	ok = write_full(ready, &address, sizeof(address)) &&
		read_full(done, &token, 1);
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int mixed_pss(void)
{
	int ready[2], done[2], status, fd;
	pid_t child;
	uintptr_t native_addr = 0;
	unsigned char *alias = MAP_FAILED;
	struct smaps_stats stats;
	int test_no = 0, failures = 0;
	bool pass;
	char fd_arg[24], ready_arg[24], done_arg[24];
	struct pollfd pollfd;
	char token = 1;

	fd = syscall(SYS_memfd_create, "mixed-smaps-pss", 0);
	if (fd < 0 || ftruncate(fd, MAPPING_SIZE) || pipe(ready) || pipe(done))
		ksft_exit_fail_msg("mixed PSS setup: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		close(ready[0]);
		close(done[1]);
		snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
		snprintf(ready_arg, sizeof(ready_arg), "%d", ready[1]);
		snprintf(done_arg, sizeof(done_arg), "%d", done[0]);
		ppps_execl(false, NULL, "--native-pss", fd_arg, ready_arg,
			   done_arg, NULL);
		_exit(EXIT_FAILURE);
	}
	close(ready[1]);
	close(done[0]);
	pollfd = (struct pollfd) { .fd = ready[0], .events = POLLIN };
	if (poll(&pollfd, 1, 10000) != 1 ||
	    !read_full(ready[0], &native_addr, sizeof(native_addr)))
		goto out;
	if (!native_addr) {
		for (test_no = 0; test_no < 3; test_no++)
			ksft_test_result_skip("native 16K process unavailable\n");
		goto out;
	}
	alias = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, fd, PROCESS_PAGE_SIZE);
	if (alias == MAP_FAILED)
		goto out;
	alias[0] = 0x35;
	pass = read_process_smaps(child, native_addr, &stats) &&
		stats_equal(&stats, 16, 14, 4, 12);
	ksft_test_result(pass, "native mapping shares only the compat-aliased slice\n");
	failures += !pass;
	test_no++;
	pass = read_smaps_stats((uintptr_t)alias, &stats) &&
		stats_equal(&stats, 4, 2, 4, 0);
	ksft_test_result(pass, "compat alias agrees with native PSS\n");
	failures += !pass;
	test_no++;
	munmap(alias, PROCESS_PAGE_SIZE);
	alias = MAP_FAILED;
	pass = read_process_smaps(child, native_addr, &stats) &&
		stats_equal(&stats, 16, 16, 0, 16);
	ksft_test_result(pass, "native private accounting recovers after compat unmap\n");
	failures += !pass;
	test_no++;
out:
	if (alias != MAP_FAILED)
		munmap(alias, PROCESS_PAGE_SIZE);
	/* EOF also releases the child if setup failed. */
	if (native_addr || test_no == 3)
		write_full(done[1], &token, 1);
	close(done[1]);
	close(ready[0]);
	close(fd);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		failures++;
	while (test_no++ < 3) {
		ksft_test_result_fail("mixed PSS setup did not complete\n");
		failures++;
	}
	return failures;
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

	ksft_print_header();
	ksft_set_plan(7);

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
	while (test_no++ < 4) {
		ksft_test_result_fail("compat PSS setup did not complete\n");
		failures++;
	}
	if (alias != MAP_FAILED)
		munmap(alias, PROCESS_PAGE_SIZE);
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);
	if (fd >= 0)
		close(fd);
	failures += mixed_pss();
	ksft_print_cnts();
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--native-pss"))
		return native_pss_child(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
	if (argc == 2 && !strcmp(argv[1], "--mixed")) {
		if (!ppps_is_compat_process())
			exec_compat(argv[0], "--mixed", NULL);
		ksft_print_header();
		ksft_set_plan(3);
		return mixed_pss() ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	if (access("/proc/self", F_OK) &&
	    mount("proc", "/proc", "proc", 0, NULL)) {
		perror("mount proc");
		return EXIT_FAILURE;
	}
	return ppps_compat_main(argc, argv, run_test);
}
