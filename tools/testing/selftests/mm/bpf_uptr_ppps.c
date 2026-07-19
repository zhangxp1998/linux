// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../bpf/test_btf.h"
#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE		4096UL
#define NATIVE_TEST_PAGE_SIZE	16384UL
#define TEST_MAPPING_SIZE	(4 * NATIVE_TEST_PAGE_SIZE)
#define MAGIC_VALUE		0x51ce4b50U

#define STR_INT		1
#define STR_USER_DATA	5
#define STR_VALUE	15
#define STR_UPTR	21
#define STR_VALUE_TYPE	26
#define STR_UDATA	37

#define RAW_INSN(CODE, DST, SRC, OFF, IMM)			\
	((struct bpf_insn) {					\
		.code = CODE,					\
		.dst_reg = DST,				\
		.src_reg = SRC,				\
		.off = OFF,					\
		.imm = IMM,					\
	})
#define MOV64_REG(DST, SRC)					\
	RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_X, DST, SRC, 0, 0)
#define MOV64_IMM(DST, IMM)					\
	RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define ADD64_IMM(DST, IMM)					\
	RAW_INSN(BPF_ALU64 | BPF_ADD | BPF_K, DST, 0, 0, IMM)
#define JEQ_IMM(DST, IMM, OFF)					\
	RAW_INSN(BPF_JMP | BPF_JEQ | BPF_K, DST, 0, OFF, IMM)
#define CALL_HELPER(ID)						\
	RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, ID)
#define LDX_MEM(SIZE, DST, SRC, OFF)				\
	RAW_INSN(BPF_LDX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define ST_MEM(SIZE, DST, OFF, IMM)				\
	RAW_INSN(BPF_ST | BPF_MEM | (SIZE), DST, 0, OFF, IMM)
#define STX_MEM(SIZE, DST, SRC, OFF)				\
	RAW_INSN(BPF_STX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define EXIT_INSN()						\
	RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

struct uptr_value {
	uint64_t udata;
};

struct uptr_btf_blob {
	struct btf_header header;
	uint32_t types[22];
	char strings[43];
};

static const struct uptr_btf_blob uptr_btf = {
	.header = {
		.magic = BTF_MAGIC,
		.version = BTF_VERSION,
		.hdr_len = sizeof(struct btf_header),
		.type_len = sizeof(uptr_btf.types),
		.str_off = sizeof(uptr_btf.types),
		.str_len = sizeof(uptr_btf.strings),
	},
	.types = {
		/* [1] int */
		BTF_TYPE_INT_ENC(STR_INT, BTF_INT_SIGNED, 0, 32, 4),
		/* [2] struct user_data { int value; } */
		BTF_STRUCT_ENC(STR_USER_DATA, 1, 4),
		BTF_MEMBER_ENC(STR_VALUE, 1, 0),
		/* [3] struct user_data __uptr */
		BTF_TYPE_TAG_ENC(STR_UPTR, 2),
		/* [4] struct user_data __uptr * */
		BTF_PTR_ENC(3),
		/* [5] struct value_type { ... *udata; } */
		BTF_STRUCT_ENC(STR_VALUE_TYPE, 1, 8),
		BTF_MEMBER_ENC(STR_UDATA, 4, 0),
	},
	.strings = "\0int\0user_data\0value\0uptr\0value_type\0udata",
};

static int sys_bpf(enum bpf_cmd command, union bpf_attr *attr)
{
	return syscall(__NR_bpf, command, attr, sizeof(*attr));
}

static int load_uptr_btf(char *log, size_t log_size)
{
	union bpf_attr attr = {
		.btf = (uintptr_t)&uptr_btf,
		.btf_size = offsetof(struct uptr_btf_blob, strings) +
			    sizeof(uptr_btf.strings),
		.btf_log_buf = (uintptr_t)log,
		.btf_log_size = log_size,
		.btf_log_level = 1,
	};

	return sys_bpf(BPF_BTF_LOAD, &attr);
}

static int create_task_storage(int btf_fd)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_TASK_STORAGE,
		.key_size = sizeof(uint32_t),
		.value_size = sizeof(struct uptr_value),
		.max_entries = 0,
		.map_flags = BPF_F_NO_PREALLOC,
		.btf_fd = btf_fd,
		.btf_key_type_id = 1,
		.btf_value_type_id = 5,
	};

	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static int create_result_map(void)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_ARRAY,
		.key_size = sizeof(uint32_t),
		.value_size = sizeof(uint32_t),
		.max_entries = 1,
	};

	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static int update_storage(int map_fd, int pidfd, const void *uptr)
{
	struct uptr_value value = {
		.udata = (uintptr_t)uptr,
	};
	union bpf_attr attr = {
		.map_fd = map_fd,
		.key = (uintptr_t)&pidfd,
		.value = (uintptr_t)&value,
		.flags = BPF_NOEXIST,
	};

	return sys_bpf(BPF_MAP_UPDATE_ELEM, &attr);
}

static int delete_storage(int map_fd, int pidfd)
{
	union bpf_attr attr = {
		.map_fd = map_fd,
		.key = (uintptr_t)&pidfd,
	};

	return sys_bpf(BPF_MAP_DELETE_ELEM, &attr);
}

static int lookup_result(int map_fd, uint32_t *result)
{
	uint32_t key = 0;
	union bpf_attr attr = {
		.map_fd = map_fd,
		.key = (uintptr_t)&key,
		.value = (uintptr_t)result,
	};

	return sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr);
}

static int load_reader_program(int storage_fd, int result_fd, char *log,
			       size_t log_size)
{
	struct bpf_insn insns[] = {
		RAW_INSN(BPF_LD | BPF_DW | BPF_IMM, BPF_REG_1,
			 BPF_PSEUDO_MAP_FD, 0, storage_fd),
		RAW_INSN(0, 0, 0, 0, 0),
		MOV64_REG(BPF_REG_6, BPF_REG_1),
		CALL_HELPER(BPF_FUNC_get_current_task_btf),
		JEQ_IMM(BPF_REG_0, 0, 17),
		MOV64_REG(BPF_REG_2, BPF_REG_0),
		MOV64_REG(BPF_REG_1, BPF_REG_6),
		MOV64_IMM(BPF_REG_3, 0),
		MOV64_IMM(BPF_REG_4, 0),
		CALL_HELPER(BPF_FUNC_task_storage_get),
		JEQ_IMM(BPF_REG_0, 0, 11),
		LDX_MEM(BPF_DW, BPF_REG_7, BPF_REG_0, 0),
		JEQ_IMM(BPF_REG_7, 0, 9),
		LDX_MEM(BPF_W, BPF_REG_8, BPF_REG_7, 0),
		ST_MEM(BPF_W, BPF_REG_10, -4, 0),
		RAW_INSN(BPF_LD | BPF_DW | BPF_IMM, BPF_REG_1,
			 BPF_PSEUDO_MAP_FD, 0, result_fd),
		RAW_INSN(0, 0, 0, 0, 0),
		MOV64_REG(BPF_REG_2, BPF_REG_10),
		ADD64_IMM(BPF_REG_2, -4),
		CALL_HELPER(BPF_FUNC_map_lookup_elem),
		JEQ_IMM(BPF_REG_0, 0, 1),
		STX_MEM(BPF_W, BPF_REG_0, BPF_REG_8, 0),
		MOV64_IMM(BPF_REG_0, 0),
		EXIT_INSN(),
	};
	static const char license[] = "GPL";
	union bpf_attr attr = {
		.prog_type = BPF_PROG_TYPE_RAW_TRACEPOINT,
		.insn_cnt = ARRAY_SIZE(insns),
		.insns = (uintptr_t)insns,
		.license = (uintptr_t)license,
		.log_buf = (uintptr_t)log,
		.log_size = log_size,
		.log_level = 2,
	};

	return sys_bpf(BPF_PROG_LOAD, &attr);
}

static int attach_sys_enter(int prog_fd)
{
	static const char tracepoint[] = "sys_enter";
	union bpf_attr attr = {
		.raw_tracepoint.name = (uintptr_t)tracepoint,
		.raw_tracepoint.prog_fd = prog_fd,
	};

	return sys_bpf(BPF_RAW_TRACEPOINT_OPEN, &attr);
}

static void *map_misaligned_memfd(int *memfd_out)
{
	void *reservation;
	uintptr_t target;
	void *mapping;
	int memfd;

	reservation = mmap(NULL, TEST_MAPPING_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	target = ((uintptr_t)reservation + NATIVE_TEST_PAGE_SIZE - 1) &
		 ~(NATIVE_TEST_PAGE_SIZE - 1);
	target += USER_PAGE_SIZE;
	munmap(reservation, TEST_MAPPING_SIZE);

	memfd = syscall(__NR_memfd_create, "bpf-uptr-ppps", MFD_CLOEXEC);
	if (memfd < 0)
		return MAP_FAILED;
	if (ftruncate(memfd, NATIVE_TEST_PAGE_SIZE)) {
		close(memfd);
		return MAP_FAILED;
	}
	mapping = mmap((void *)target, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_FIXED, memfd, 0);
	if (mapping == MAP_FAILED) {
		close(memfd);
		return MAP_FAILED;
	}
	*memfd_out = memfd;
	return mapping;
}

static int run_test(void)
{
	struct rlimit memlock = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	char btf_log[65536] = {};
	char verifier_log[65536] = {};
	void *cross_mapping = MAP_FAILED;
	void *slice_mapping = MAP_FAILED;
	uintptr_t native_base;
	void *cross_uptr;
	uint32_t result = 0;
	uint32_t magic = MAGIC_VALUE;
	bool cross_rejected = false;
	bool valid_updated = false;
	bool reader_loaded = false;
	int storage_fd = -1;
	int result_fd = -1;
	int btf_fd = -1;
	int pidfd = -1;
	int prog_fd = -1;
	int link_fd = -1;
	int memfd = -1;
	int ret;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	setrlimit(RLIMIT_MEMLOCK, &memlock);

	btf_fd = load_uptr_btf(btf_log, sizeof(btf_log));
	storage_fd = btf_fd >= 0 ? create_task_storage(btf_fd) : -1;
	result_fd = create_result_map();
	pidfd = syscall(__NR_pidfd_open, getpid(), 0);
	ksft_test_result(btf_fd >= 0 && storage_fd >= 0 && result_fd >= 0 &&
			 pidfd >= 0, "create BTF and BPF maps\n");
	if (storage_fd < 0) {
		ksft_print_msg("BTF setup: btf=%d storage=%d result=%d pidfd=%d errno=%s\n",
			       btf_fd, storage_fd, result_fd, pidfd,
			       strerror(errno));
		ksft_print_msg("%s\n", btf_log);
		goto out;
	}

	cross_mapping = mmap(NULL, TEST_MAPPING_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cross_mapping != MAP_FAILED) {
		native_base = ((uintptr_t)cross_mapping +
			       NATIVE_TEST_PAGE_SIZE - 1) &
			      ~(NATIVE_TEST_PAGE_SIZE - 1);
		cross_uptr = (void *)(native_base + 2 * USER_PAGE_SIZE - 2);
		memcpy(cross_uptr, &magic, sizeof(magic));
		errno = 0;
		ret = update_storage(storage_fd, pidfd, cross_uptr);
		cross_rejected = ret < 0 && errno == EOPNOTSUPP;
		if (!ret)
			delete_storage(storage_fd, pidfd);
	}
	ksft_test_result(cross_mapping != MAP_FAILED,
			 "map an object across a process-page boundary\n");
	ksft_test_result(cross_rejected,
			 "reject an uptr spanning two process pages\n");

	slice_mapping = map_misaligned_memfd(&memfd);
	ksft_test_result(slice_mapping != MAP_FAILED &&
			 ((uintptr_t)slice_mapping &
			  (NATIVE_TEST_PAGE_SIZE - 1)) == USER_PAGE_SIZE,
			 "map file slice at a mismatched native-page offset\n");
	if (slice_mapping != MAP_FAILED) {
		memcpy(slice_mapping, &magic, sizeof(magic));
		ret = update_storage(storage_fd, pidfd, slice_mapping);
		valid_updated = ret == 0;
	}
	ksft_test_result(valid_updated,
			 "pin a single-process-page uptr\n");

	if (valid_updated) {
		prog_fd = load_reader_program(storage_fd, result_fd,
					      verifier_log,
					      sizeof(verifier_log));
		if (prog_fd >= 0)
			link_fd = attach_sys_enter(prog_fd);
		reader_loaded = prog_fd >= 0 && link_fd >= 0;
	}
	ksft_test_result(reader_loaded, "load and attach the uptr reader\n");
	if (!reader_loaded && valid_updated)
		ksft_print_msg("BPF reader failed: %s\n%s\n",
			       strerror(errno), verifier_log);
	if (reader_loaded) {
		syscall(__NR_getpid);
		lookup_result(result_fd, &result);
	}
	ksft_test_result(result == magic,
			 "BPF reads the registered process-page slice\n");

out:
	if (link_fd >= 0)
		close(link_fd);
	if (prog_fd >= 0)
		close(prog_fd);
	if (valid_updated)
		delete_storage(storage_fd, pidfd);
	if (slice_mapping != MAP_FAILED)
		munmap(slice_mapping, USER_PAGE_SIZE);
	if (memfd >= 0)
		close(memfd);
	if (cross_mapping != MAP_FAILED)
		munmap(cross_mapping, TEST_MAPPING_SIZE);
	if (pidfd >= 0)
		close(pidfd);
	if (result_fd >= 0)
		close(result_fd);
	if (storage_fd >= 0)
		close(storage_fd);
	if (btf_fd >= 0)
		close(btf_fd);
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
	execl("/proc/self/exe", "bpf_uptr_ppps", "--run", NULL);
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
