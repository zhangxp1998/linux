// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) userfaultfd handler receives a 4K compat owner's shmem minor
 * fault and resolves it with a one-page UFFDIO_CONTINUE; the owner's fault
 * then resumes with the shmem contents.
 */
#define _GNU_SOURCE

#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define RESERVE_SIZE		(3 * PROCESS_PAGE_SIZE)
#define OWNER_RESERVE_ADDRESS	((1UL << 28) + 7 * PROCESS_PAGE_SIZE)
#define BACKING_VALUE		0x6d

struct handler_report {
	long page_size;
	long poll_result;
	ssize_t read_result;
	__u64 event;
	__u64 flags;
	unsigned long address;
	int continue_result;
	int continue_error;
	__s64 updated;
};

struct fault_args {
	unsigned char *destination;
	int completion_fd;
};

static void *fault_thread(void *opaque)
{
	struct fault_args *args = opaque;
	unsigned char value = *args->destination;

	write_full(args->completion_fd, &value, sizeof(value));
	return NULL;
}

static int run_native_handler(int uffd, unsigned long destination,
			      int ready_fd, int report_fd)
{
	struct handler_report report = {
		.page_size = sysconf(_SC_PAGESIZE),
		.poll_result = -1,
		.read_result = -1,
		.continue_result = -1,
	};
	struct uffdio_continue continuation = {
		.range.start = destination,
		.range.len = PROCESS_PAGE_SIZE,
	};
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	struct uffd_msg message = {};
	char ready = 1;

	if (!write_full(ready_fd, &ready, sizeof(ready)))
		return EXIT_FAILURE;
	close(ready_fd);

	report.poll_result = poll(&pollfd, 1, 2000);
	if (report.poll_result > 0 && pollfd.revents & POLLIN) {
		report.read_result = read(uffd, &message, sizeof(message));
		if (report.read_result == sizeof(message)) {
			report.event = message.event;
			report.flags = message.arg.pagefault.flags;
			report.address = message.arg.pagefault.address;
		}
	}

	if (report.event == UFFD_EVENT_PAGEFAULT) {
		errno = 0;
		report.continue_result = ioctl(uffd, UFFDIO_CONTINUE,
					       &continuation);
		report.continue_error = errno;
		report.updated = continuation.mapped;
	}
	if (!write_full(report_fd, &report, sizeof(report)))
		return EXIT_FAILURE;

	return report.poll_result > 0 &&
	       report.read_result == sizeof(message) &&
	       report.event == UFFD_EVENT_PAGEFAULT &&
	       report.flags & UFFD_PAGEFAULT_FLAG_MINOR &&
	       report.address == destination && !report.continue_result &&
	       report.updated == PROCESS_PAGE_SIZE ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int exec_native_handler(int uffd, unsigned long destination,
			       int ready_fd, int report_fd)
{
	char destination_arg[32];
	char ready_fd_arg[16];
	char report_fd_arg[16];
	char uffd_arg[16];

	snprintf(uffd_arg, sizeof(uffd_arg), "%d", uffd);
	snprintf(destination_arg, sizeof(destination_arg), "%lu", destination);
	snprintf(ready_fd_arg, sizeof(ready_fd_arg), "%d", ready_fd);
	snprintf(report_fd_arg, sizeof(report_fd_arg), "%d", report_fd);
	ppps_execl(false, NULL, "--handler", uffd_arg, destination_arg,
		   ready_fd_arg, report_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_compat_owner(void)
{
	struct uffdio_register registration = {};
	struct uffdio_api api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_MINOR_SHMEM,
	};
	struct handler_report report = {};
	struct fault_args fault_args;
	unsigned char *destination;
	unsigned char *reservation;
	unsigned char *alias;
	unsigned char observed = 0;
	int completion_pipe[2];
	int handler_status = 0;
	int ready_pipe[2];
	int report_pipe[2];
	bool handler_ready = false;
	bool owner_resumed = false;
	bool report_received;
	bool setup_ok;
	pthread_t thread;
	pid_t handler;
	int memfd;
	int uffd;
	char ready;

	ppps_require_compat();
	ksft_print_header();
	ksft_set_plan(9);

	memfd = memfd_create("uffd-minor-ppps", MFD_CLOEXEC);
	alias = memfd < 0 || ftruncate(memfd, PROCESS_PAGE_SIZE) ? MAP_FAILED :
		mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED, memfd, 0);
	if (alias != MAP_FAILED) {
		*alias = BACKING_VALUE;
		munmap(alias, PROCESS_PAGE_SIZE);
	}
	reservation = mmap((void *)OWNER_RESERVE_ADDRESS, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	destination = reservation == MAP_FAILED ? MAP_FAILED :
		mmap(reservation + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd, 0);
	uffd = syscall(SYS_userfaultfd, O_NONBLOCK);
	setup_ok = alias != MAP_FAILED && destination != MAP_FAILED && uffd >= 0 &&
		   !ioctl(uffd, UFFDIO_API, &api) &&
		   api.features & UFFD_FEATURE_MINOR_SHMEM;
	if (setup_ok) {
		registration.range.start = (unsigned long)destination;
		registration.range.len = PROCESS_PAGE_SIZE;
		registration.mode = UFFDIO_REGISTER_MODE_MINOR;
		setup_ok = !ioctl(uffd, UFFDIO_REGISTER, &registration) &&
			   registration.ioctls & (1ULL << _UFFDIO_CONTINUE);
	}
	ksft_test_result(setup_ok, "register one shmem page for UFFD minor\n");
	if (!setup_ok) {
		if (uffd < 0 && errno == EPERM)
			ksft_exit_skip("userfaultfd unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd minor setup failed: %s\n",
				   strerror(errno));
	}

	if (pipe(ready_pipe) || pipe(report_pipe) || pipe(completion_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	handler = fork();
	if (handler < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!handler) {
		close(ready_pipe[0]);
		close(report_pipe[0]);
		close(completion_pipe[0]);
		close(completion_pipe[1]);
		_exit(exec_native_handler(uffd, (unsigned long)destination,
					  ready_pipe[1], report_pipe[1]));
	}

	close(ready_pipe[1]);
	close(report_pipe[1]);
	handler_ready = read_full(ready_pipe[0], &ready, sizeof(ready));
	close(ready_pipe[0]);
	ksft_test_result(handler_ready,
			 "start a native UFFD minor handler\n");

	fault_args.destination = destination;
	fault_args.completion_fd = completion_pipe[1];
	if (pthread_create(&thread, NULL, fault_thread, &fault_args))
		ksft_exit_fail_msg("pthread_create failed\n");

	report_received = read_full(report_pipe[0], &report, sizeof(report));
	close(report_pipe[0]);
	waitpid(handler, &handler_status, 0);
	ksft_print_msg("owner page size=%ld, handler page size=%ld\n",
		       sysconf(_SC_PAGESIZE), report.page_size);
	ksft_test_result(report_received &&
			 (report.page_size == PROCESS_PAGE_SIZE ||
			  report.page_size == 4 * PROCESS_PAGE_SIZE),
			 "handler reports a valid native page size\n");
	ksft_test_result(report_received && report.poll_result > 0 &&
			 report.read_result == sizeof(struct uffd_msg) &&
			 report.event == UFFD_EVENT_PAGEFAULT,
			 "native handler receives the owner minor fault\n");
	ksft_test_result(report_received &&
			 report.flags & UFFD_PAGEFAULT_FLAG_MINOR,
			 "fault carries UFFD_PAGEFAULT_FLAG_MINOR\n");
	ksft_test_result(report_received &&
			 report.address == (unsigned long)destination,
			 "fault address belongs to the 4K owner\n");
	ksft_test_result(report_received && !report.continue_result &&
			 report.updated == PROCESS_PAGE_SIZE,
			 "native handler continues one owner page (%s, %lld bytes)\n",
			 report.continue_result ? strerror(report.continue_error) : "ok",
			 (long long)report.updated);

	if (report_received) {
		struct pollfd completion = {
			.fd = completion_pipe[0],
			.events = POLLIN,
		};

		owner_resumed = poll(&completion, 1, 500) > 0 &&
				completion.revents & POLLIN &&
				read_full(completion_pipe[0], &observed,
					  sizeof(observed));
	}
	ksft_test_result(owner_resumed && observed == BACKING_VALUE,
			 "owner fault resumes with the shmem contents\n");
	ksft_test_result(WIFEXITED(handler_status) &&
			 WEXITSTATUS(handler_status) == EXIT_SUCCESS,
			 "native handler completes cleanly\n");

	close(uffd);
	if (!owner_resumed) {
		struct pollfd completion = {
			.fd = completion_pipe[0],
			.events = POLLIN,
		};

		if (poll(&completion, 1, 1000) > 0) {
			ssize_t ignored = read(completion_pipe[0], &observed,
					       sizeof(observed));

			(void)ignored;
		}
	}
	pthread_join(thread, NULL);
	close(completion_pipe[1]);
	close(completion_pipe[0]);
	close(memfd);
	munmap(reservation, RESERVE_SIZE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	const char *mode = ppps_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], "--owner", NULL);
	if (argc == 2 && !strcmp(mode, "--owner"))
		return run_compat_owner();
	if (argc == 6 && !strcmp(mode, "--handler"))
		return run_native_handler(atoi(argv[2]), strtoul(argv[3], NULL, 10),
					  atoi(argv[4]), atoi(argv[5]));
	return EXIT_FAILURE;
}
