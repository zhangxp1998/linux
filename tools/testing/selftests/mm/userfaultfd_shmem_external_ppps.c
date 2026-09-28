// SPDX-License-Identifier: GPL-2.0
/* External shmem users adopt native backing-page population. */
#define _GNU_SOURCE
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "kselftest_ppps.h"

struct fixture {
	int fd, uffd;
	unsigned char *map, *source;
};

static void copy_slice(struct fixture *f, size_t offset)
{
	struct uffdio_copy copy = {
		.src = (uintptr_t)f->source + offset,
		.dst = (uintptr_t)f->map + offset,
		.len = PROCESS_PAGE_SIZE,
	};

	if (ioctl(f->uffd, UFFDIO_COPY, &copy) ||
	    copy.copy != PROCESS_PAGE_SIZE)
		ksft_exit_fail_msg("COPY setup: %s\n", strerror(errno));
}

static void setup(struct fixture *f)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };

	f->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (f->uffd < 0)
		ksft_exit_skip("userfaultfd unavailable: %s\n", strerror(errno));
	if (ioctl(f->uffd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API: %s\n", strerror(errno));
	if (!(api.features & UFFD_FEATURE_MISSING_SHMEM))
		ksft_exit_skip("UFFD shmem missing mode unavailable\n");
	/* Inherited only by the native writer. */
	f->fd = memfd_create("ppps-external", MFD_ALLOW_SEALING);
	if (f->fd < 0 || ftruncate(f->fd, NATIVE_PAGE_SIZE) ||
	    fcntl(f->fd, F_ADD_SEALS, F_SEAL_SHRINK))
		ksft_exit_fail_msg("memfd setup: %s\n", strerror(errno));
	f->map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, f->fd, 0);
	f->source = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (f->map == MAP_FAILED || f->source == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	memset(f->source, 0, NATIVE_PAGE_SIZE);
	memset(f->source, 0x51, PROCESS_PAGE_SIZE);
	reg.range.start = (uintptr_t)f->map;
	reg.range.len = NATIVE_PAGE_SIZE;
	if (ioctl(f->uffd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	copy_slice(f, 0);
}

static void cleanup(struct fixture *f)
{
	munmap(f->map, NATIVE_PAGE_SIZE);
	munmap(f->source, NATIVE_PAGE_SIZE);
	close(f->fd);
	close(f->uffd);
}

struct reader {
	const unsigned char *source;
	unsigned char *dest;
	size_t size;
	int done;
};

static void *read_bytes(void *arg)
{
	struct reader *r = arg;

	memcpy(r->dest, r->source, r->size);
	if (write(r->done, "x", 1) != 1)
		_exit(1);
	return NULL;
}

/* Bound waits, and resolve any unexpected event so a failure cannot hang. */
static bool check_read(struct fixture *f, size_t offset, size_t size,
		       bool expect_missing)
{
	unsigned char actual[NATIVE_PAGE_SIZE];
	struct pollfd pollfds[2];
	struct reader reader;
	bool missing = false;
	pthread_t thread;
	int done[2];

	if (pipe(done))
		ksft_exit_fail_msg("pipe failed\n");
	reader = (struct reader){ f->map + offset, actual, size, done[1] };
	pollfds[0] = (struct pollfd){ .fd = f->uffd, .events = POLLIN };
	pollfds[1] = (struct pollfd){ .fd = done[0], .events = POLLIN };
	if (pthread_create(&thread, NULL, read_bytes, &reader))
		ksft_exit_fail_msg("pthread_create failed\n");
	for (;;) {
		struct uffd_msg msg;
		size_t address;

		if (poll(pollfds, 2, 5000) <= 0)
			ksft_exit_fail_msg("timed out waiting for mapped read\n");
		if (pollfds[0].revents & POLLIN) {
			if (read(f->uffd, &msg, sizeof(msg)) != sizeof(msg) ||
			    msg.event != UFFD_EVENT_PAGEFAULT)
				ksft_exit_fail_msg("unexpected UFFD event\n");
			address = msg.arg.pagefault.address & ~(PROCESS_PAGE_SIZE - 1);
			if (address < (uintptr_t)f->map ||
			    address >= (uintptr_t)f->map + NATIVE_PAGE_SIZE)
				ksft_exit_fail_msg("unexpected fault address\n");
			missing = true;
			copy_slice(f, address - (uintptr_t)f->map);
		}
		if (pollfds[1].revents & POLLIN)
			break;
	}
	pthread_join(thread, NULL);
	close(done[0]);
	close(done[1]);
	return missing == expect_missing &&
	       !memcmp(actual, f->source + offset, size);
}

static bool reject_copy(struct fixture *f, size_t offset)
{
	struct uffdio_copy copy = {
		.src = (uintptr_t)f->source + offset,
		.dst = (uintptr_t)f->map + offset,
		.len = PROCESS_PAGE_SIZE,
	};

	return ioctl(f->uffd, UFFDIO_COPY, &copy) == -1 &&
	       errno == EEXIST && copy.copy == -EEXIST;
}

static int native_write(int fd)
{
	unsigned char bytes[PROCESS_PAGE_SIZE];

	if (getpagesize() != NATIVE_PAGE_SIZE)
		return 1;
	memset(bytes, 0x72, sizeof(bytes));
	return pwrite(fd, bytes, sizeof(bytes), PROCESS_PAGE_SIZE) != sizeof(bytes);
}

static void external_access(struct fixture *f, int kind)
{
	unsigned char byte, *alias;
	int pipefd[2], status, device, buffer;
	struct udmabuf_create create = {
		.memfd = f->fd, .size = NATIVE_PAGE_SIZE,
		.flags = UDMABUF_FLAGS_CLOEXEC,
	};
	pid_t pid;
	char fdarg[32];

	switch (kind) {
	case 0:
		snprintf(fdarg, sizeof(fdarg), "%d", f->fd);
		pid = fork();
		if (pid == 0) {
			ppps_execl(false, NULL, "--writer", fdarg, NULL);
			_exit(1);
		}
		if (pid < 0 || waitpid(pid, &status, 0) != pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			ksft_exit_fail_msg("native writer failed\n");
		memset(f->source + PROCESS_PAGE_SIZE, 0x72, PROCESS_PAGE_SIZE);
		break;
	case 1:
		if (pread(f->fd, &byte, 1, 0) != 1 || byte != 0x51)
			ksft_exit_fail_msg("pread failed\n");
		break;
	case 2:
		if (pipe(pipefd) || splice(f->fd, NULL, pipefd[1], NULL, 1, 0) != 1 ||
		    read(pipefd[0], &byte, 1) != 1 || byte != 0x51)
			ksft_exit_fail_msg("splice failed\n");
		close(pipefd[0]);
		close(pipefd[1]);
		break;
	case 3:
		if (fallocate(f->fd, 0, 0, NATIVE_PAGE_SIZE))
			ksft_exit_fail_msg("fallocate failed\n");
		break;
	case 4:
		alias = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, f->fd, 0);
		if (alias == MAP_FAILED || *(volatile unsigned char *)alias != 0x51)
			ksft_exit_fail_msg("unregistered alias read failed\n");
		munmap(alias, NATIVE_PAGE_SIZE);
		break;
	case 5:
		device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
		if (device < 0)
			ksft_exit_fail_msg("udmabuf open: %s\n", strerror(errno));
		buffer = ioctl(device, UDMABUF_CREATE, &create);
		if (buffer < 0)
			ksft_exit_fail_msg("udmabuf pin: %s\n", strerror(errno));
		close(buffer);
		close(device);
		break;
	}
}

static int run_test(void)
{
	const char *names[] = { "native pwrite", "pread", "splice", "fallocate",
				"unregistered mmap", "cached memfd pin" };
	struct fixture f;
	size_t i;

	ksft_print_header();
	ksft_set_plan(14);
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		setup(&f);
		external_access(&f, i);
		/* Check the cache state, not COPY's existing-PTE rejection. */
		if (madvise(f.map, NATIVE_PAGE_SIZE, MADV_DONTNEED))
			ksft_exit_fail_msg("MADV_DONTNEED failed\n");
		ksft_test_result(reject_copy(&f, 2 * PROCESS_PAGE_SIZE),
				 "%s: sibling cache slice rejects COPY\n", names[i]);
		ksft_test_result(check_read(&f, 0, NATIVE_PAGE_SIZE, false),
				 "%s: initialized backing reads without missing\n", names[i]);
		cleanup(&f);
	}
	setup(&f);
	if (madvise(f.map, PROCESS_PAGE_SIZE, MADV_DONTNEED))
		ksft_exit_fail_msg("MADV_DONTNEED failed\n");
	ksft_test_result(check_read(&f, 0, PROCESS_PAGE_SIZE, false),
			 "UFFD filled-slice refault stays present\n");
	ksft_test_result(check_read(&f, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE, true),
			 "UFFD refault preserves sibling missing state\n");
	cleanup(&f);
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--writer"))
		return native_write(atoi(argv[2]));
	return ppps_compat_main(argc, argv, run_test);
}
