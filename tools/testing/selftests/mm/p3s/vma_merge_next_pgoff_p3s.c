// SPDX-License-Identifier: GPL-2.0
/*
 * Changing the tail of a VMA so that it merges with the following VMA
 * shrinks the first VMA and moves the start of the second one backwards
 * (vma_merge_existing_range(), "merge right" with __adjust_next_start).  The
 * second VMA's vm_pgoff must then be decreased by the number of pages it
 * gained.  If it is computed from an address below vm_start with unsigned
 * arithmetic it wraps, and the moved VMA no longer maps the file offsets or
 * anon_vma indices it covers: file faults in it raise SIGBUS and it can no
 * longer merge with its linear neighbours.
 *
 * The bug is a regression for native 16K processes on a P3S kernel, so the
 * native geometry is tested first; the 4KB geometry must behave the
 * same way.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"
#include "vm_util.h"

#define UNITS	3

static sigjmp_buf fault_env;

static void fault_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

/* Read one byte, returning the signal number on a fault and 0 otherwise. */
static int read_byte(const volatile unsigned char *addr, unsigned char *val)
{
	int sig = sigsetjmp(fault_env, 1);

	if (sig)
		return sig;
	*val = *addr;
	return 0;
}

/* The range is covered by exactly the listed VMA boundaries. */
static bool vmas_are(unsigned char *base, size_t unit, int nr_expected,
		     const int *boundaries)
{
	struct procmap_fd procmap;
	bool ok = true;
	int i;

	if (open_self_procmap(&procmap))
		return false;
	for (i = 0; i < nr_expected; i++) {
		uintptr_t start = (uintptr_t)base + boundaries[i] * unit;
		uintptr_t end = (uintptr_t)base + boundaries[i + 1] * unit;

		if (!find_vma_procmap(&procmap, (void *)start) ||
		    procmap.query.vma_start != start ||
		    procmap.query.vma_end != end) {
			ok = false;
			break;
		}
	}
	close_procmap(&procmap);
	return ok;
}

/*
 * Map UNITS pages, make the last one read-only, then make the middle one
 * read-only so that it is cut from the first VMA and merged into the last.
 * Returns 0 on pass, 1 on fail.
 */
static int check_merge_next(const char *name, int fd, size_t unit)
{
	static const int split[] = { 0, 1, UNITS };
	static const int whole[] = { 0, UNITS };
	unsigned char *base, value = 0;
	int flags = fd >= 0 ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS;
	unsigned int i;
	int sig, ret = 0;

	base = mmap(NULL, UNITS * unit, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (base == MAP_FAILED) {
		ksft_print_msg("%s: mmap: %s\n", name, strerror(errno));
		return 1;
	}
	for (i = 0; i < UNITS; i++)
		memset(base + i * unit, 0x41 + i, unit);

	if (mprotect(base + 2 * unit, unit, PROT_READ) ||
	    mprotect(base + unit, unit, PROT_READ)) {
		ksft_print_msg("%s: mprotect: %s\n", name, strerror(errno));
		ret = 1;
		goto out;
	}
	if (!vmas_are(base, unit, 2, split)) {
		ksft_print_msg("%s: the read-only middle page did not merge into the next VMA\n",
			       name);
		ret = 1;
		goto out;
	}

	if (fd >= 0) {
		/* Drop the PTEs so the moved range has to fault from the file. */
		if (madvise(base + unit, 2 * unit, MADV_DONTNEED)) {
			ksft_print_msg("%s: MADV_DONTNEED: %s\n", name,
				       strerror(errno));
			ret = 1;
			goto out;
		}
	}
	for (i = 0; i < UNITS; i++) {
		sig = read_byte(base + i * unit + unit / 2, &value);
		if (sig) {
			ksft_print_msg("%s: reading page %u raised signal %d\n",
				       name, i, sig);
			ret = 1;
		} else if (value != 0x41 + i) {
			ksft_print_msg("%s: page %u reads %#x, expected %#x\n",
				       name, i, value, 0x41 + i);
			ret = 1;
		}
	}

	/* Linear vm_pgoff on both sides lets the three pages merge again. */
	if (mprotect(base, unit, PROT_READ)) {
		ksft_print_msg("%s: mprotect head: %s\n", name, strerror(errno));
		ret = 1;
		goto out;
	}
	if (!vmas_are(base, unit, 1, whole)) {
		ksft_print_msg("%s: linear neighbours no longer merge\n", name);
		ret = 1;
	}
out:
	munmap(base, UNITS * unit);
	return ret;
}

static int run_case(bool file)
{
	size_t unit = getpagesize();
	struct sigaction action = { .sa_handler = fault_handler };
	char name[64];
	int fd = -1, ret;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL) ||
	    sigaction(SIGSEGV, &action, NULL))
		return KSFT_FAIL;
	if (file) {
		fd = syscall(__NR_memfd_create, "vma_merge_next_pgoff", 0);
		if (fd < 0 || ftruncate(fd, UNITS * unit)) {
			ksft_print_msg("memfd: %s\n", strerror(errno));
			if (fd >= 0)
				close(fd);
			return KSFT_FAIL;
		}
	}
	snprintf(name, sizeof(name), "%zuK %s", unit / 1024,
		 file ? "shmem file" : "anon");
	ret = check_merge_next(name, fd, unit);
	if (fd >= 0)
		close(fd);
	return ret ? KSFT_FAIL : KSFT_PASS;
}

#define CHECK_GEOMETRY(four_kb, file) do { \
	int ret = p3s_run_child(four_kb, four_kb ? "--4kb" : "--check-native", \
				file ? "file" : "anon", NULL); \
	if (ret == KSFT_SKIP) \
		SKIP(return, "requested geometry unavailable"); \
	EXPECT_EQ(KSFT_PASS, ret); \
} while (0)

TEST(native_anon_merge_right_keeps_linear_pgoff)
{
	CHECK_GEOMETRY(false, false);
}

TEST(native_file_merge_right_keeps_linear_pgoff)
{
	CHECK_GEOMETRY(false, true);
}

TEST(four_kb_anon_merge_right_keeps_linear_pgoff)
{
	CHECK_GEOMETRY(true, false);
}

TEST(four_kb_file_merge_right_keeps_linear_pgoff)
{
	CHECK_GEOMETRY(true, true);
}

static int child_role(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--check-native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE)
			return KSFT_SKIP;
		return run_case(!strcmp(argv[2], "file"));
	}
	if (argc == 3 && !strcmp(argv[1], "--4kb")) {
		if (!p3s_is_4kb_process())
			return KSFT_SKIP;
		return run_case(!strcmp(argv[2], "file"));
	}
	return -1;
}

P3S_HARNESS_4KB_MAIN
