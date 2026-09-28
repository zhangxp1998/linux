// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process must be able to mmap() a whole buffer exported with
 * dma_mmap_attrs() / dma_mmap_pages(), not only a quarter of it: the fixture
 * buffer is one native 16KB page whose 4KB slices hold marker, marker + 1, ...
 * Mapping all four slices and the native control must succeed and show their
 * markers.  A 4KB process must reject an upper-half VMA at offset 8KB, and a
 * native mapping longer than the buffer must fail without populating anything.
 *
 * The fixture is selected on the command line: "attrs" or "pages".
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_SETUP setup_harness
#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#include "p3s_harness.h"

#define BUFFER_SIZE	NATIVE_PAGE_SIZE

struct fixture {
	const char *name;
	const char *device;
	unsigned char marker;
};

static const struct fixture fixtures[] = {
	{ "attrs", "/dev/dma_mmap_attrs_p3s", 0x61 },
	{ "pages", "/dev/dma_mmap_pages_p3s", 0x51 },
};

static sigjmp_buf fault_env;

static void fault_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

static int probe(const volatile unsigned char *addr, unsigned char *value)
{
	int sig = sigsetjmp(fault_env, 1);

	if (sig)
		return sig;
	*value = *addr;
	return 0;
}

/* Every 4K slice of [map, map + len) holds marker + first_slice + i. */
static bool check_slices(const unsigned char *map, size_t len,
			 unsigned char marker, unsigned int first_slice,
			 const char *what)
{
	size_t off;
	unsigned char value;
	bool ok = true;

	for (off = 0; off < len; off += PROCESS_PAGE_SIZE) {
		unsigned char want = marker + first_slice + off / PROCESS_PAGE_SIZE;
		int sig = probe(map + off, &value);

		if (sig) {
			ksft_print_msg("%s: offset %zu raised signal %d\n", what,
				       off, sig);
			ok = false;
		} else if (value != want) {
			ksft_print_msg("%s: offset %zu reads %#x, expected %#x\n",
				       what, off, value, want);
			ok = false;
		}
	}
	return ok;
}

static bool map_and_check(int fd, size_t len, off_t offset,
			  unsigned char marker, const char *what)
{
	unsigned char *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, offset);
	bool ok;

	if (map == MAP_FAILED) {
		ksft_print_msg("%s: mmap(%zu bytes at %lld): %s\n", what, len,
			       (long long)offset, strerror(errno));
		return false;
	}
	ok = check_slices(map, len, marker, offset / PROCESS_PAGE_SIZE, what);
	munmap(map, len);
	return ok;
}

static bool offset_rejected(int fd, size_t len, off_t offset)
{
	void *map;
	int error;

	errno = 0;
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, offset);
	error = errno;
	if (map != MAP_FAILED)
		munmap(map, len);
	return map == MAP_FAILED && error == EINVAL;
}

/* A mapping past the end of the buffer fails and leaves no PTEs behind. */
static bool oversized_rejected(int fd, const char *what)
{
	size_t len = BUFFER_SIZE + NATIVE_PAGE_SIZE;
	unsigned char *reservation, *map, value;
	bool ok = true;
	size_t off;

	reservation = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
			   -1, 0);
	if (reservation == MAP_FAILED)
		return false;
	map = mmap(reservation, len, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
	if (map != MAP_FAILED) {
		ksft_print_msg("%s: a %zu-byte mapping of a %lu-byte buffer succeeded\n",
			       what, len, BUFFER_SIZE);
		munmap(map, len);
		return false;
	}
	/* The failed mmap() unmapped the reservation; nothing may be readable. */
	for (off = 0; off < len; off += PROCESS_PAGE_SIZE) {
		if (!probe(reservation + off, &value)) {
			ksft_print_msg("%s: offset %zu readable after failed mmap\n",
				       what, off);
			ok = false;
		}
	}
	return ok;
}

static int run_case(const struct fixture *fixture, const char *test)
{
	struct sigaction action = { .sa_handler = fault_handler };
	int fd, ret = KSFT_FAIL;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL) ||
	    sigaction(SIGBUS, &action, NULL))
		return KSFT_FAIL;
	fd = p3s_open_fixture(fixture->device, O_RDWR);
	if (fd < 0) {
		ksft_print_msg("%s: %s\n", fixture->device, strerror(errno));
		return p3s_fixture_unavailable(errno) ? KSFT_SKIP : KSFT_FAIL;
	}
	if (!strcmp(test, "whole"))
		ret = map_and_check(fd, BUFFER_SIZE, 0, fixture->marker,
				    "whole buffer") ? KSFT_PASS : KSFT_FAIL;
	else if (!strcmp(test, "offset"))
		ret = p3s_is_4kb_process() &&
		      offset_rejected(fd, BUFFER_SIZE / 2, BUFFER_SIZE / 2) ?
		      KSFT_PASS : KSFT_FAIL;
	else if (!strcmp(test, "oversized"))
		ret = oversized_rejected(fd, "oversized") ? KSFT_PASS : KSFT_FAIL;
	close(fd);
	return ret;
}

static const struct fixture *find_fixture(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fixtures); i++)
		if (!strcmp(fixtures[i].name, name))
			return &fixtures[i];
	return NULL;
}

static const struct fixture *selected_fixture;

static int run_child(bool four_kb, const char *name, const char *test)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return KSFT_FAIL;
	if (!pid) {
		p3s_execl(four_kb, NULL, four_kb ? "--4kb" : "--native",
			  name, test, NULL);
		_exit(KSFT_FAIL);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return KSFT_FAIL;
	return WEXITSTATUS(status);
}

#define CHECK_CHILD(four_kb, test) do { \
	int ret = run_child(four_kb, selected_fixture->name, test); \
	if (ret == KSFT_SKIP) \
		SKIP(return, "%s fixture unavailable", selected_fixture->name); \
	EXPECT_EQ(KSFT_PASS, ret); \
} while (0)

TEST(native_process_maps_whole_dma_buffer)
{
	CHECK_CHILD(false, "whole");
}

TEST(four_kb_process_maps_all_four_slices)
{
	CHECK_CHILD(true, "whole");
}

TEST(four_kb_rejects_upper_half_offset)
{
	CHECK_CHILD(true, "offset");
}

TEST(native_rejects_oversized_mapping_without_ptes)
{
	CHECK_CHILD(false, "oversized");
}

static int child_role(int argc, char **argv)
{
	const struct fixture *fixture;

	if (argc != 4 || (strcmp(argv[1], "--native") &&
			  strcmp(argv[1], "--4kb")))
		return -1;
	fixture = find_fixture(argv[2]);
	if (!fixture)
		return KSFT_FAIL;
	if (!strcmp(argv[1], "--native"))
		return p3s_is_4kb_process() ? KSFT_FAIL :
		       run_case(fixture, argv[3]);
	return p3s_is_4kb_process() ? run_case(fixture, argv[3]) :
		       KSFT_FAIL;
}

static void setup_harness(int *argc, char ***argv)
{
	char *name = p3s_harness_take_positional(argc, argv);

	selected_fixture = name ? find_fixture(name) : NULL;
	if (!selected_fixture && !p3s_harness_listing(*argc, *argv))
		ksft_exit_fail_msg("usage: %s attrs|pages\n", (*argv)[0]);
}

P3S_HARNESS_4KB_MAIN
