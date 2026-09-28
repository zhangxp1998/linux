// SPDX-License-Identifier: GPL-2.0
/* Native truncation must invalidate complete 4KB tail slices in remote VMAs. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define FILE_BYTES (3 * NATIVE_PAGE_SIZE)

struct layout {
	unsigned long offset;
	unsigned long eof;
};

static const struct layout layouts[] = {
	{ 0, PROCESS_PAGE_SIZE },
	{ 0, PROCESS_PAGE_SIZE + 1 },
	{ 0, 2 * PROCESS_PAGE_SIZE },
	{ PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE },
	{ NATIVE_PAGE_SIZE + PROCESS_PAGE_SIZE,
	  NATIVE_PAGE_SIZE + 2 * PROCESS_PAGE_SIZE },
	{ 0, NATIVE_PAGE_SIZE },
	{ 3 * PROCESS_PAGE_SIZE, NATIVE_PAGE_SIZE },
};

struct report {
	long page_size;
	bool initialized;
	bool old_mapping;
	bool fresh_mapping;
};

static sigjmp_buf fault_env;

static void sigbus_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

static bool read_byte(const unsigned char *p, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *(const volatile unsigned char *)p;
	return true;
}

static bool check_mapping(unsigned char *p, unsigned long offset,
			  unsigned long eof, bool cow, long page_size)
{
	unsigned long accessible = (eof + page_size - 1) & ~(page_size - 1);
	bool ok = true;
	unsigned long i;

	for (i = 0; i < NATIVE_PAGE_SIZE; i += PROCESS_PAGE_SIZE) {
		unsigned char value = 0;
		unsigned long file_pos = offset + i;
		bool readable = read_byte(p + i, &value);

		if (file_pos >= accessible) {
			ok &= !readable;
			continue;
		}
		ok &= readable;
		if (file_pos < eof)
			ok &= value == 0x40 + file_pos / PROCESS_PAGE_SIZE +
				      (cow ? 0x10 : 0);
	}
	return ok;
}

static int run_child(int fd, int output, int input, unsigned long offset,
		     unsigned long eof, bool cow)
{
	struct sigaction action = { .sa_handler = sigbus_handler };
	struct report report = { .page_size = sysconf(_SC_PAGESIZE) };
	unsigned char *mapping, *fresh;
	unsigned long i;
	char token;

	/* Keep native controls aligned; compatibility cases retain slice offsets. */
	if (report.page_size == NATIVE_PAGE_SIZE)
		offset &= ~(NATIVE_PAGE_SIZE - 1);
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       cow ? MAP_PRIVATE : MAP_SHARED, fd, offset);
	report.initialized = mapping != MAP_FAILED;
	if (report.initialized) {
		for (i = 0; i < NATIVE_PAGE_SIZE; i += PROCESS_PAGE_SIZE) {
			report.initialized &= mapping[i] ==
				0x40 + (offset + i) / PROCESS_PAGE_SIZE;
			if (cow)
				mapping[i] += 0x10;
		}
	}
	if (!write_full(output, &report, sizeof(report)) ||
	    !read_full(input, &token, 1))
		return EXIT_FAILURE;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL))
		return EXIT_FAILURE;
	if (report.initialized) {
		report.old_mapping = check_mapping(mapping, offset, eof, cow,
						   report.page_size);
		fresh = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
			     offset);
		if (fresh != MAP_FAILED) {
			report.fresh_mapping = check_mapping(fresh, offset, eof,
							     false, report.page_size);
			munmap(fresh, NATIVE_PAGE_SIZE);
		}
		munmap(mapping, NATIVE_PAGE_SIZE);
	}
	if (!write_full(output, &report, sizeof(report)))
		return EXIT_FAILURE;
	return report.old_mapping && report.fresh_mapping ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool run_case(struct __test_metadata *_metadata, const char *dir, bool compat, bool memfd, bool cow,
		     const struct layout *layout)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	int output[2], input[2], fd, status;
	struct report report = {};
	char path[512], fd_arg[16], out_arg[16], in_arg[16];
	char offset_arg[32], eof_arg[32];
	bool ok;
	pid_t child;
	unsigned int i;
	char token = 1;

	ASSERT_LT(snprintf(path, sizeof(path), "%s/truncate-tail-XXXXXX", dir),
		  (int)sizeof(path));
	fd = memfd ? memfd_create("truncate-tail", 0) : mkstemp(path);
	ASSERT_GE(fd, 0);
	if (!memfd)
		ASSERT_EQ(0, unlink(path));
	for (i = 0; i < FILE_BYTES / PROCESS_PAGE_SIZE; i++) {
		memset(page, 0x40 + i, sizeof(page));
		ASSERT_EQ(sizeof(page), pwrite(fd, page, sizeof(page), i * sizeof(page)));
	}
	ASSERT_EQ(0, pipe(output));
	ASSERT_EQ(0, pipe(input));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		close(output[0]);
		close(input[1]);
		snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
		snprintf(out_arg, sizeof(out_arg), "%d", output[1]);
		snprintf(in_arg, sizeof(in_arg), "%d", input[0]);
		snprintf(offset_arg, sizeof(offset_arg), "%lu", layout->offset);
		snprintf(eof_arg, sizeof(eof_arg), "%lu", layout->eof);
		p3s_execl(compat, NULL, "--child", fd_arg, out_arg, in_arg,
			  offset_arg, eof_arg, cow ? "private" : "shared", NULL);
		_exit(EXIT_FAILURE);
	}
	close(output[1]);
	close(input[0]);
	ok = read_full(output[0], &report, sizeof(report)) && report.initialized &&
		report.page_size == (compat ? PROCESS_PAGE_SIZE : NATIVE_PAGE_SIZE);
	ok &= !ftruncate(fd, layout->eof);
	ok &= write_full(input[1], &token, 1);
	close(input[1]);
	ok &= read_full(output[0], &report, sizeof(report)) &&
		report.old_mapping && report.fresh_mapping;
	close(output[0]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ok &= WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
	close(fd);
	return ok;
}

static const char *test_dir;

FIXTURE(truncate_tail) { };

FIXTURE_VARIANT(truncate_tail) {
	bool compat;
	bool memfd;
	bool cow;
};

FIXTURE_VARIANT_ADD(truncate_tail, native_file_shared) { };
FIXTURE_VARIANT_ADD(truncate_tail, native_file_private) { .cow = true };
FIXTURE_VARIANT_ADD(truncate_tail, native_memfd_shared) { .memfd = true };
FIXTURE_VARIANT_ADD(truncate_tail, native_memfd_private) {
	.memfd = true, .cow = true,
};
FIXTURE_VARIANT_ADD(truncate_tail, compat_file_shared) { .compat = true };
FIXTURE_VARIANT_ADD(truncate_tail, compat_file_private) {
	.compat = true, .cow = true,
};
FIXTURE_VARIANT_ADD(truncate_tail, compat_memfd_shared) {
	.compat = true, .memfd = true,
};
FIXTURE_VARIANT_ADD(truncate_tail, compat_memfd_private) {
	.compat = true, .memfd = true, .cow = true,
};

FIXTURE_SETUP(truncate_tail)
{
}

FIXTURE_TEARDOWN(truncate_tail)
{
}

TEST_F(truncate_tail, eof_after_first_slice)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[0]));
}

TEST_F(truncate_tail, eof_one_byte_into_second_slice)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[1]));
}

TEST_F(truncate_tail, eof_after_second_slice)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[2]));
}

TEST_F(truncate_tail, offset_one_slice_eof_after_second)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[3]));
}

TEST_F(truncate_tail, offset_in_second_native_page)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[4]));
}

TEST_F(truncate_tail, eof_after_first_native_page)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[5]));
}

TEST_F(truncate_tail, slice_before_native_boundary_retains_bytes)
{
	EXPECT_TRUE(run_case(_metadata, test_dir, variant->compat, variant->memfd,
			     variant->cow, &layouts[6]));
}

static int child_role(int argc, char **argv)
{
	if (argc != 8 || strcmp(argv[1], "--child"))
		return -1;
	alarm(60);
	signal(SIGPIPE, SIG_IGN);
	return run_child(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
			 strtoul(argv[5], NULL, 0), strtoul(argv[6], NULL, 0),
			 !strcmp(argv[7], "private"));
}

static void setup_harness(int *argc, char ***argv)
{
	alarm(60);
	signal(SIGPIPE, SIG_IGN);
	test_dir = p3s_harness_take_positional(argc, argv);
	if (!test_dir)
		test_dir = ".";
}

P3S_HARNESS_4KB_MAIN
