// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define EXTENDED_SIZE (USER_PAGE_SIZE + 1)

#define BPF_RAW_INSN(CODE, DST, SRC, OFF, IMM) \
	((struct bpf_insn){ .code = CODE, .dst_reg = DST, .src_reg = SRC, \
			    .off = OFF, .imm = IMM })
#define BPF_MOV64_IMM(DST, IMM) \
	BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_MOV64_REG(DST, SRC) \
	BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_X, DST, SRC, 0, 0)
#define BPF_ALU64_IMM(OP, DST, IMM) \
	BPF_RAW_INSN(BPF_ALU64 | OP | BPF_K, DST, 0, 0, IMM)
#define BPF_ST_MEM(SIZE, DST, OFF, IMM) \
	BPF_RAW_INSN(BPF_ST | BPF_MEM | SIZE, DST, 0, OFF, IMM)
#define BPF_JMP_IMM(OP, DST, IMM, OFF) \
	BPF_RAW_INSN(BPF_JMP | OP | BPF_K, DST, 0, OFF, IMM)
#define BPF_CALL_HELPER(ID) \
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, ID)
#define BPF_EXIT_INSN() \
	BPF_RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int create_map(void)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_ARRAY,
		.key_size = sizeof(unsigned int),
		.value_size = EXTENDED_SIZE,
		.max_entries = 1,
	};
	unsigned int key = 0;
	char value[EXTENDED_SIZE] = {};
	int fd;

	fd = sys_bpf(BPF_MAP_CREATE, &attr);
	if (fd < 0)
		ksft_exit_fail_msg("BPF_MAP_CREATE failed: %s\n",
				   strerror(errno));
	memcpy(value, "ppps", 5);
	memset(&attr, 0, sizeof(attr));
	attr.map_fd = fd;
	attr.key = (unsigned long)&key;
	attr.value = (unsigned long)value;
	attr.flags = BPF_ANY;
	if (sys_bpf(BPF_MAP_UPDATE_ELEM, &attr))
		ksft_exit_fail_msg("BPF_MAP_UPDATE_ELEM failed: %s\n",
				   strerror(errno));
	return fd;
}

static int load_program(int map_fd)
{
	static const char license[] = "GPL";
	struct bpf_insn insns[] = {
		BPF_MOV64_REG(BPF_REG_6, BPF_REG_1),
		BPF_ST_MEM(BPF_W, BPF_REG_10, -4, 0),
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
		BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
		BPF_RAW_INSN(BPF_LD | BPF_DW | BPF_IMM, BPF_REG_1,
			     BPF_PSEUDO_MAP_FD, 0, map_fd),
		BPF_RAW_INSN(0, 0, 0, 0, 0),
		BPF_CALL_HELPER(BPF_FUNC_map_lookup_elem),
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 7),
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_0),
		BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
		BPF_MOV64_IMM(BPF_REG_3, EXTENDED_SIZE),
		BPF_CALL_HELPER(BPF_FUNC_sysctl_set_new_value),
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 2),
		BPF_MOV64_IMM(BPF_REG_0, 1),
		BPF_EXIT_INSN(),
		BPF_MOV64_IMM(BPF_REG_0, 0),
		BPF_EXIT_INSN(),
	};
	char log[16384] = {};
	union bpf_attr attr = {
		.prog_type = BPF_PROG_TYPE_CGROUP_SYSCTL,
		.insn_cnt = sizeof(insns) / sizeof(insns[0]),
		.insns = (unsigned long)insns,
		.license = (unsigned long)license,
		.log_buf = (unsigned long)log,
		.log_size = sizeof(log),
		.log_level = 1,
		.expected_attach_type = BPF_CGROUP_SYSCTL,
	};
	int fd = sys_bpf(BPF_PROG_LOAD, &attr);

	if (fd < 0)
		ksft_exit_fail_msg("BPF_PROG_LOAD failed: %s\n%s\n",
				   strerror(errno), log);
	return fd;
}

static void attach_program(int cgroup_fd, int prog_fd)
{
	union bpf_attr attr = {
		.target_fd = cgroup_fd,
		.attach_bpf_fd = prog_fd,
		.attach_type = BPF_CGROUP_SYSCTL,
	};

	if (sys_bpf(BPF_PROG_ATTACH, &attr))
		ksft_exit_fail_msg("BPF_PROG_ATTACH failed: %s\n",
				   strerror(errno));
}

static int run_test(void)
{
	const char *cgroup_path = getenv("CGROUP_MOUNT");
	char hostname[256];
	union bpf_attr attr = {};
	int cgroup_fd;
	int hostname_fd;
	ssize_t hostname_len;
	int saved_errno;
	int map_fd;
	int prog_fd;
	ssize_t ret;

	ksft_print_header();
	ksft_set_plan(2);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	hostname_fd = open("/proc/sys/kernel/hostname", O_RDONLY);
	if (hostname_fd < 0)
		ksft_exit_fail_msg("open hostname failed: %s\n", strerror(errno));
	hostname_len = read(hostname_fd, hostname, sizeof(hostname));
	close(hostname_fd);
	if (hostname_len <= 0)
		ksft_exit_fail_msg("read hostname failed: %s\n", strerror(errno));

	if (!cgroup_path)
		cgroup_path = "/sys/fs/cgroup";
	cgroup_fd = open(cgroup_path, O_RDONLY | O_DIRECTORY);
	if (cgroup_fd < 0)
		ksft_exit_fail_msg("open cgroup failed: %s\n", strerror(errno));
	map_fd = create_map();
	prog_fd = load_program(map_fd);
	attach_program(cgroup_fd, prog_fd);

	hostname_fd = open("/proc/sys/kernel/hostname", O_WRONLY);
	if (hostname_fd < 0)
		ksft_exit_fail_msg("open hostname for write failed: %s\n",
				   strerror(errno));
	errno = 0;
	ret = write(hostname_fd, hostname, hostname_len);
	saved_errno = errno;
	ksft_print_msg("sysctl write returned %zd, errno %d (%s)\n", ret,
		       saved_errno, strerror(saved_errno));
	ksft_test_result(ret == hostname_len,
			 "bound cgroup BPF sysctl values to the process page\n");

	attr.target_fd = cgroup_fd;
	attr.attach_type = BPF_CGROUP_SYSCTL;
	sys_bpf(BPF_PROG_DETACH, &attr);
	close(hostname_fd);
	close(prog_fd);
	close(map_fd);
	close(cgroup_fd);
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
	execl("/proc/self/exe", "bpf_sysctl_size_ppps", "--run", NULL);
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
