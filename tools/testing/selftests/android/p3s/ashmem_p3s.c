// SPDX-License-Identifier: GPL-2.0
/* Pin a shared ashmem region using each caller's process-page geometry. */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE native_pin_role
#include "p3s_harness.h"

#define ASHMEM_NOT_PURGED	0
#define ASHMEM_WAS_PURGED	1
#define ASHMEM_IS_UNPINNED	0
#define ASHMEM_IS_PINNED	1

struct ashmem_pin {
	__u32 offset;
	__u32 len;
};

#define __ASHMEMIOC		0x77
#define ASHMEM_SET_SIZE		_IOW(__ASHMEMIOC, 3, size_t)
#define ASHMEM_PIN		_IOW(__ASHMEMIOC, 7, struct ashmem_pin)
#define ASHMEM_UNPIN		_IOW(__ASHMEMIOC, 8, struct ashmem_pin)
#define ASHMEM_GET_PIN_STATUS	_IO(__ASHMEMIOC, 9)
#define ASHMEM_PURGE_ALL_CACHES	_IO(__ASHMEMIOC, 10)

static int new_region(void)
{
	int fd = p3s_open_fixture("/dev/ashmem", O_RDWR);

	if (fd >= 0 && ioctl(fd, ASHMEM_SET_SIZE, 2 * NATIVE_PAGE_SIZE)) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

static bool pin_status(int fd, unsigned int command, unsigned int length,
		       int expected)
{
	struct ashmem_pin pin = { .offset = 0, .len = length };

	return ioctl(fd, command, &pin) == expected;
}

/* len == 0 covers the native-rounded extent accepted by ashmem mmap. */
static int tail_range_checks(void)
{
	const unsigned long map_size = 2 * NATIVE_PAGE_SIZE;
	struct ashmem_pin whole = { .offset = 0, .len = 0 };
	struct ashmem_pin tail = {
		.offset = NATIVE_PAGE_SIZE, .len = NATIVE_PAGE_SIZE,
	};
	unsigned char resident = 0;
	unsigned char *mapping;
	int fd, unpin, status, purge, purge_err, repin, cache;
	bool zero, ok;

	fd = p3s_open_fixture("/dev/ashmem", O_RDWR);
	if (fd < 0)
		return KSFT_FAIL;
	if (ioctl(fd, ASHMEM_SET_SIZE, NATIVE_PAGE_SIZE + PROCESS_PAGE_SIZE)) {
		close(fd);
		return KSFT_FAIL;
	}
	mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return KSFT_FAIL;
	}
	mapping[NATIVE_PAGE_SIZE] = 0x5a;
	ok = !mincore(mapping + NATIVE_PAGE_SIZE, getpagesize(), &resident) &&
	     (resident & 1);
	unpin = ioctl(fd, ASHMEM_UNPIN, &whole);
	status = ioctl(fd, ASHMEM_GET_PIN_STATUS, &tail);
	errno = 0;
	purge = ioctl(fd, ASHMEM_PURGE_ALL_CACHES);
	purge_err = errno;
	if (purge < 0 && purge_err == EPERM) {
		munmap(mapping, map_size);
		close(fd);
		return ok && unpin == 0 && status == ASHMEM_IS_UNPINNED ?
			KSFT_SKIP : KSFT_FAIL;
	}
	resident = 0xff;
	cache = mincore(mapping + NATIVE_PAGE_SIZE, getpagesize(), &resident);
	repin = ioctl(fd, ASHMEM_PIN, &whole);
	zero = mapping[NATIVE_PAGE_SIZE] == 0;
	ksft_print_msg("ashmem tail pagesz=%d unpin=%d status=%d purge=%d "
		       "mincore=%d resident=%u repin=%d zero=%d\n",
		       getpagesize(), unpin, status, purge, cache, resident, repin, zero);
	ok &= unpin == 0 && status == ASHMEM_IS_UNPINNED && purge >= 0 &&
	      cache == 0 && !(resident & 1) && repin == ASHMEM_WAS_PURGED && zero;
	munmap(mapping, map_size);
	close(fd);
	return ok ? KSFT_PASS : KSFT_FAIL;
}

static int native_checks(int fd)
{
	struct ashmem_pin first = { .offset = 0, .len = PROCESS_PAGE_SIZE };
	bool own = fd < 0;
	bool ok;
	void *mapping;

	if (getpagesize() != NATIVE_PAGE_SIZE)
		return EXIT_FAILURE;
	if (own)
		fd = new_region();
	if (fd < 0)
		return EXIT_FAILURE;
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		if (own)
			close(fd);
		return EXIT_FAILURE;
	}
	munmap(mapping, NATIVE_PAGE_SIZE);
	errno = 0;
	ok = ioctl(fd, ASHMEM_UNPIN, &first) == -1 && errno == EINVAL;
	ok &= pin_status(fd, ASHMEM_UNPIN, NATIVE_PAGE_SIZE, 0);
	ok &= pin_status(fd, ASHMEM_GET_PIN_STATUS, NATIVE_PAGE_SIZE,
			 ASHMEM_IS_UNPINNED);
	ok &= pin_status(fd, ASHMEM_PIN, NATIVE_PAGE_SIZE, ASHMEM_NOT_PURGED);
	ok &= pin_status(fd, ASHMEM_GET_PIN_STATUS, NATIVE_PAGE_SIZE,
			 ASHMEM_IS_PINNED);
	if (own) {
		close(fd);
		if (tail_range_checks() == KSFT_FAIL)
			ok = false;
	}
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool native_child(int fd)
{
	char fd_arg[16];
	int flags = fd < 0 ? 0 : fcntl(fd, F_GETFD);
	int status;
	pid_t pid;

	if (fd >= 0 && (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC)))
		return false;
	snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
	pid = fork();
	if (!pid) {
		p3s_execl(false, NULL, "--native-pin", fd_arg, NULL);
		_exit(EXIT_FAILURE);
	}
	if (fd >= 0 && fcntl(fd, F_SETFD, flags))
		return false;
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

FIXTURE(ashmem_region) {
	int fd;
};

FIXTURE_SETUP(ashmem_region)
{
	void *mapping;

	self->fd = -1;
	self->fd = new_region();
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "/dev/ashmem is unavailable");
	ASSERT_GE(self->fd, 0);
	/* Ashmem must have a mapped backing file before pin operations. */
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_EQ(0, munmap(mapping, NATIVE_PAGE_SIZE));
}

FIXTURE_TEARDOWN(ashmem_region)
{
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(ashmem_region, compat_unpins_4k_range)
{
	EXPECT_TRUE(pin_status(self->fd, ASHMEM_UNPIN, PROCESS_PAGE_SIZE, 0));
}

TEST_F(ashmem_region, compat_queries_unpinned_4k_range)
{
	ASSERT_TRUE(pin_status(self->fd, ASHMEM_UNPIN,
				    PROCESS_PAGE_SIZE, 0));
	EXPECT_TRUE(pin_status(self->fd, ASHMEM_GET_PIN_STATUS,
				   PROCESS_PAGE_SIZE, ASHMEM_IS_UNPINNED));
}

TEST_F(ashmem_region, compat_pins_4k_range)
{
	ASSERT_TRUE(pin_status(self->fd, ASHMEM_UNPIN,
				    PROCESS_PAGE_SIZE, 0));
	EXPECT_TRUE(pin_status(self->fd, ASHMEM_PIN,
				   PROCESS_PAGE_SIZE, ASHMEM_NOT_PURGED));
}

TEST_F(ashmem_region, compat_queries_pinned_4k_range)
{
	EXPECT_TRUE(pin_status(self->fd, ASHMEM_GET_PIN_STATUS,
				   PROCESS_PAGE_SIZE, ASHMEM_IS_PINNED));
}

TEST_F(ashmem_region, native_caller_pins_shared_fd)
{
	ASSERT_TRUE(pin_status(self->fd, ASHMEM_UNPIN,
				    PROCESS_PAGE_SIZE, 0));
	EXPECT_TRUE(native_child(self->fd));
}

TEST_F(ashmem_region, compat_observes_native_caller_pin)
{
	ASSERT_TRUE(pin_status(self->fd, ASHMEM_UNPIN,
				    PROCESS_PAGE_SIZE, 0));
	ASSERT_TRUE(native_child(self->fd));
	EXPECT_TRUE(pin_status(self->fd, ASHMEM_GET_PIN_STATUS,
				   PROCESS_PAGE_SIZE, ASHMEM_IS_PINNED));
}

TEST_F(ashmem_region, native_pin_unpin_status_semantics)
{
	EXPECT_TRUE(native_child(-1));
}

TEST_F(ashmem_region, zero_length_unpin_reclaims_native_rounded_tail)
{
	int ret = tail_range_checks();

	if (ret == KSFT_SKIP)
		SKIP(return, "ASHMEM_PURGE_ALL_CACHES requires privileges");
	EXPECT_EQ(KSFT_PASS, ret);
}

static int native_pin_role(int argc, char **argv)
{
	char *end;
	long fd;

	if (argc == 3 && !strcmp(argv[1], "--native-pin")) {
		errno = 0;
		fd = strtol(argv[2], &end, 10);
		if (errno || *end || fd < -1 || fd > INT_MAX)
			return EXIT_FAILURE;
		return native_checks(fd);
	}
	return -1;
}

P3S_HARNESS_4KB_MAIN
