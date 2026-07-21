// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define SCSI_IOCTL_SEND_COMMAND 1
#define USER_PAGE_SIZE 4096UL
#define MAPPING_SIZE 16384UL
#define OVERSIZED_TRANSFER (USER_PAGE_SIZE + 1)

struct scsi_ioctl_command {
	unsigned int inlen;
	unsigned int outlen;
	unsigned char data[];
};

static int run_test(void)
{
	const char *device = getenv("SCSI_DEVICE");
	struct scsi_ioctl_command *command;
	int saved_errno;
	int fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	if (!device)
		device = "/dev/sda";
	fd = open(device, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("cannot open SCSI block device %s: %s\n",
			       device, strerror(errno));

	command = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (command == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	if (mprotect((char *)command + USER_PAGE_SIZE, USER_PAGE_SIZE,
		     PROT_NONE))
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	command->inlen = OVERSIZED_TRANSFER;
	command->outlen = 0;
	command->data[0] = 0;
	errno = 0;
	ret = ioctl(fd, SCSI_IOCTL_SEND_COMMAND, command);
	saved_errno = errno;
	ksft_print_msg("SCSI_IOCTL_SEND_COMMAND returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound legacy SCSI transfers to the process page\n");

	munmap(command, MAPPING_SIZE);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("could not enable 4K compatibility mode\n");
	execl("/proc/self/exe", "scsi_ioctl_size_ppps", "--compat", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--compat"))
		return run_test();
	return EXIT_FAILURE;
}
