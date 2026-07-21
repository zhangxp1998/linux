// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

#define BPF_RAW_INSN(CODE, DST, SRC, OFF, IMM) \
	((struct bpf_insn){ .code = CODE, .dst_reg = DST, .src_reg = SRC, \
			    .off = OFF, .imm = IMM })
#define BPF_MOV64_IMM(DST, IMM) \
	BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_EXIT_INSN() \
	BPF_RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)
#define BPF_LDX_MEM(SIZE, DST, SRC, OFF) \
	BPF_RAW_INSN(BPF_LDX | BPF_MEM | SIZE, DST, SRC, OFF, 0)
#define BPF_ALU64_REG(OP, DST, SRC) \
	BPF_RAW_INSN(BPF_ALU64 | OP | BPF_X, DST, SRC, 0, 0)
#define BPF_STX_MEM(SIZE, DST, SRC, OFF) \
	BPF_RAW_INSN(BPF_STX | BPF_MEM | SIZE, DST, SRC, OFF, 0)

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int load_allow_program(enum bpf_attach_type attach_type,
			      bool measure_optlen)
{
	static const char license[] = "GPL";
	struct bpf_insn insns[7];
	char log[8192] = {};
	union bpf_attr attr;
	int count = 0;
	int fd;

	if (measure_optlen) {
		insns[count++] = BPF_LDX_MEM(BPF_DW, BPF_REG_2, BPF_REG_1,
					  offsetof(struct bpf_sockopt, optval_end));
		insns[count++] = BPF_LDX_MEM(BPF_DW, BPF_REG_3, BPF_REG_1,
					  offsetof(struct bpf_sockopt, optval));
		insns[count++] = BPF_ALU64_REG(BPF_SUB, BPF_REG_2, BPF_REG_3);
		insns[count++] = BPF_STX_MEM(BPF_W, BPF_REG_1, BPF_REG_2,
					  offsetof(struct bpf_sockopt, optlen));
	}
	insns[count++] = BPF_MOV64_IMM(BPF_REG_0, 1);
	insns[count++] = BPF_EXIT_INSN();

	attr = (union bpf_attr) {
		.prog_type = BPF_PROG_TYPE_CGROUP_SOCKOPT,
		.insn_cnt = count,
		.insns = (unsigned long)insns,
		.license = (unsigned long)license,
		.log_buf = (unsigned long)log,
		.log_size = sizeof(log),
		.log_level = 1,
		.expected_attach_type = attach_type,
	};
	fd = sys_bpf(BPF_PROG_LOAD, &attr);
	if (fd < 0)
		ksft_exit_fail_msg("BPF_PROG_LOAD failed: %s\n%s\n",
				   strerror(errno), log);
	return fd;
}

static void attach_program(int cgroup_fd, int prog_fd,
			   enum bpf_attach_type attach_type)
{
	union bpf_attr attr = {
		.target_fd = cgroup_fd,
		.attach_bpf_fd = prog_fd,
		.attach_type = attach_type,
	};

	if (sys_bpf(BPF_PROG_ATTACH, &attr))
		ksft_exit_fail_msg("BPF_PROG_ATTACH failed: %s\n",
				   strerror(errno));
}

static void detach_program(int cgroup_fd, enum bpf_attach_type attach_type)
{
	union bpf_attr attr = {
		.target_fd = cgroup_fd,
		.attach_type = attach_type,
	};

	sys_bpf(BPF_PROG_DETACH, &attr);
}

static int run_test(void)
{
	const char *cgroup_path = getenv("CGROUP_MOUNT");
	void *optval;
	int cgroup_fd;
	int get_prog_fd;
	int set_prog_fd;
	int saved_errno;
	int sock_fd;
	socklen_t optlen;
	int ret;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	if (!cgroup_path)
		cgroup_path = "/sys/fs/cgroup";
	cgroup_fd = open(cgroup_path, O_RDONLY | O_DIRECTORY);
	if (cgroup_fd < 0)
		ksft_exit_fail_msg("open cgroup failed: %s\n", strerror(errno));

	set_prog_fd = load_allow_program(BPF_CGROUP_SETSOCKOPT, false);
	get_prog_fd = load_allow_program(BPF_CGROUP_GETSOCKOPT, true);
	attach_program(cgroup_fd, set_prog_fd, BPF_CGROUP_SETSOCKOPT);
	attach_program(cgroup_fd, get_prog_fd, BPF_CGROUP_GETSOCKOPT);

	optval = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (optval == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	if (mprotect((char *)optval + USER_PAGE_SIZE, USER_PAGE_SIZE,
		     PROT_NONE))
		ksft_exit_fail_msg("mprotect failed: %s\n", strerror(errno));

	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0)
		ksft_exit_fail_msg("socket failed: %s\n", strerror(errno));

	errno = 0;
	ret = setsockopt(sock_fd, SOL_SOCKET, 0x7fffffff, optval,
			 EXTENDED_SIZE);
	saved_errno = errno;
	ksft_print_msg("setsockopt returned %d, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == ENOPROTOOPT,
			 "bound cgroup BPF setsockopt input to the process page\n");

	optlen = EXTENDED_SIZE;
	errno = 0;
	ret = getsockopt(sock_fd, SOL_SOCKET, SO_TYPE, optval, &optlen);
	saved_errno = errno;
	ksft_print_msg("getsockopt returned %d, errno %d (%s), optlen %u\n",
		       ret, saved_errno, strerror(saved_errno), optlen);
	ksft_test_result(ret == 0 && optlen == USER_PAGE_SIZE,
			 "bound cgroup BPF getsockopt output to the process page\n");

	detach_program(cgroup_fd, BPF_CGROUP_GETSOCKOPT);
	detach_program(cgroup_fd, BPF_CGROUP_SETSOCKOPT);
	close(sock_fd);
	close(get_prog_fd);
	close(set_prog_fd);
	close(cgroup_fd);
	munmap(optval, NATIVE_PAGE_SIZE);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "bpf_sockopt_size_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
