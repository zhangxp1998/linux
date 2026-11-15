// SPDX-License-Identifier: GPL-2.0
/*
 * A BPF_F_STACK_BUILD_ID stack trace captured from a 4KB process resolves the
 * build ID of code mapped from file offset 4KB and reports an offset inside
 * that 4KB file slice.
 */
#define _GNU_SOURCE

#include <elf.h>
#include <linux/bpf.h>
#include <linux/memfd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define FILE_SIZE		(2 * PROCESS_PAGE_SIZE)
#define CODE_FILE_OFFSET	PROCESS_PAGE_SIZE
#define NOTE_OFFSET		0x100
#define STACK_DEPTH		16
#define STACK_ENTRIES		64
#define TRIGGER_COUNT		64
#define STACK_CAPTURE_ATTEMPTS 16

#define RAW_INSN(CODE, DST, SRC, OFF, IMM)			\
	((struct bpf_insn) {					\
		.code = CODE,					\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off = OFF,					\
		.imm = IMM,					\
	})
#define MOV64_REG(DST, SRC)					\
	RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_X, DST, SRC, 0, 0)
#define MOV64_IMM(DST, IMM)					\
	RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define RSH64_IMM(DST, IMM)					\
	RAW_INSN(BPF_ALU64 | BPF_RSH | BPF_K, DST, 0, 0, IMM)
#define JNE_IMM(DST, IMM, OFF)					\
	RAW_INSN(BPF_JMP | BPF_JNE | BPF_K, DST, 0, OFF, IMM)
#define CALL_HELPER(ID)						\
	RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, ID)
#define EXIT_INSN()						\
	RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)
static const unsigned char expected_build_id[BPF_BUILD_ID_SIZE] = {
	0x50, 0x50, 0x50, 0x53, 0x2d, 0x62, 0x70, 0x66, 0x2d, 0x73,
	0x74, 0x61, 0x63, 0x6b, 0x2d, 0x69, 0x64, 0x2d, 0x30, 0x31,
};

static int sys_bpf(enum bpf_cmd command, union bpf_attr *attr)
{
	return syscall(__NR_bpf, command, attr, sizeof(*attr));
}

static int create_stack_map(void)
{
	union bpf_attr attr = {
		.map_type = BPF_MAP_TYPE_STACK_TRACE,
		.key_size = sizeof(uint32_t),
		.value_size = STACK_DEPTH * sizeof(struct bpf_stack_build_id),
		.max_entries = STACK_ENTRIES,
		.map_flags = BPF_F_STACK_BUILD_ID,
	};

	return sys_bpf(BPF_MAP_CREATE, &attr);
}

static int load_capture_program(int map_fd, uint32_t tgid, char *log,
				size_t log_size)
{
	struct bpf_insn insns[] = {
		MOV64_REG(BPF_REG_6, BPF_REG_1),
		CALL_HELPER(BPF_FUNC_get_current_pid_tgid),
		RSH64_IMM(BPF_REG_0, 32),
		JNE_IMM(BPF_REG_0, tgid, 9),
		/*
		 * Only capture the payload's getpid. Our map lookups can hash
		 * to the same stack ID and replace it with BPF_F_REUSE_STACKID.
		 */
		RAW_INSN(BPF_LDX | BPF_DW | BPF_MEM, BPF_REG_0, BPF_REG_6,
			 offsetof(struct bpf_raw_tracepoint_args, args[1]), 0),
		JNE_IMM(BPF_REG_0, __NR_getpid, 7),
		MOV64_REG(BPF_REG_1, BPF_REG_6),
		RAW_INSN(BPF_LD | BPF_DW | BPF_IMM, BPF_REG_2,
			 BPF_PSEUDO_MAP_FD, 0, map_fd),
		RAW_INSN(0, 0, 0, 0, 0),
		MOV64_IMM(BPF_REG_3,
			  BPF_F_USER_STACK | BPF_F_REUSE_STACKID),
		CALL_HELPER(BPF_FUNC_get_stackid),
		MOV64_IMM(BPF_REG_0, 0),
		EXIT_INSN(),
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
		.log_level = 1,
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

static int lookup_stack(int map_fd, uint32_t key,
			struct bpf_stack_build_id *stack)
{
	union bpf_attr attr = {
		.map_fd = map_fd,
		.key = (uintptr_t)&key,
		.value = (uintptr_t)stack,
	};

	return sys_bpf(BPF_MAP_LOOKUP_ELEM, &attr);
}

static int create_test_elf(void)
{
	unsigned char image[FILE_SIZE] = {};
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)image;
	Elf64_Phdr *phdr = (Elf64_Phdr *)(image + sizeof(*ehdr));
	Elf64_Nhdr *nhdr = (Elf64_Nhdr *)(image + NOTE_OFFSET);
	unsigned char *note_name = (unsigned char *)(nhdr + 1);
	unsigned char *note_desc = note_name + 4;
	int fd;

	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS64;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_type = ET_DYN;
	ehdr->e_machine = EM_AARCH64;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phoff = sizeof(*ehdr);
	ehdr->e_phentsize = sizeof(*phdr);
	ehdr->e_phnum = 1;

	phdr->p_type = PT_NOTE;
	phdr->p_offset = NOTE_OFFSET;
	phdr->p_filesz = sizeof(*nhdr) + 4 + sizeof(expected_build_id);
	phdr->p_memsz = phdr->p_filesz;
	phdr->p_align = 4;

	nhdr->n_namesz = 4;
	nhdr->n_descsz = sizeof(expected_build_id);
	nhdr->n_type = NT_GNU_BUILD_ID;
	memcpy(note_name, "GNU", 4);
	memcpy(note_desc, expected_build_id, sizeof(expected_build_id));

	/* mov x8, #__NR_getpid; svc #0; ret */
	((uint32_t *)(image + CODE_FILE_OFFSET))[0] =
		0xd2800000U | ((__NR_getpid & 0xffffU) << 5) | 8U;
	((uint32_t *)(image + CODE_FILE_OFFSET))[1] = 0xd4000001U;
	((uint32_t *)(image + CODE_FILE_OFFSET))[2] = 0xd65f03c0U;

	fd = memfd_create("bpf-stack-build-id-p3s", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (write(fd, image, sizeof(image)) != sizeof(image)) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool find_test_build_id(int map_fd, uint64_t *reported_offset,
			       unsigned int *stack_count)
{
	struct bpf_stack_build_id stack[STACK_DEPTH];
	uint32_t key;
	unsigned int frame;
	bool found = false;

	*stack_count = 0;
	for (key = 0; key < STACK_ENTRIES; key++) {
		memset(stack, 0, sizeof(stack));
		if (lookup_stack(map_fd, key, stack))
			continue;
		(*stack_count)++;
		for (frame = 0; frame < STACK_DEPTH; frame++) {
			if (stack[frame].status == BPF_STACK_BUILD_ID_EMPTY)
				break;
			if (stack[frame].status == BPF_STACK_BUILD_ID_VALID &&
			    !memcmp(stack[frame].build_id, expected_build_id,
				    sizeof(expected_build_id))) {
				*reported_offset = stack[frame].offset;
				found = true;
			}
		}
	}
	return found;
}

TEST(p3s_bpf_stack_build_id)
{
	typedef long (*trigger_fn_t)(void);
	struct rlimit memlock = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	char verifier_log[65536] = {};
	uint64_t reported_offset = UINT64_MAX;
	unsigned int stack_count = 0;
	trigger_fn_t trigger;
	void *code;
	bool build_id_found = false;
	bool offset_ok;
	int stack_fd;
	int prog_fd;
	int link_fd;
	int elf_fd;
	int saved_errno;
	int i;
	int attempt;

	setrlimit(RLIMIT_MEMLOCK, &memlock);

	elf_fd = create_test_elf();
	ASSERT_GE(elf_fd, 0);
	code = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE,
		    elf_fd, CODE_FILE_OFFSET);
	if (code == MAP_FAILED) {
		close(elf_fd);
		ASSERT_NE(MAP_FAILED, code);
	}
	__builtin___clear_cache(code, (char *)code + PROCESS_PAGE_SIZE);
	trigger = (trigger_fn_t)code;

	stack_fd = create_stack_map();
	if (stack_fd < 0) {
		saved_errno = errno;
		munmap(code, PROCESS_PAGE_SIZE);
		close(elf_fd);
		if (saved_errno == EPERM || saved_errno == EACCES)
			SKIP(return, "BPF stack trace requires privilege: %s",
			     strerror(saved_errno));
		errno = saved_errno;
		ASSERT_GE(stack_fd, 0);
	}

	prog_fd = load_capture_program(stack_fd, getpid(), verifier_log,
				       sizeof(verifier_log));
	if (prog_fd < 0) {
		saved_errno = errno;
		close(stack_fd);
		munmap(code, PROCESS_PAGE_SIZE);
		close(elf_fd);
		if (saved_errno == EPERM || saved_errno == EACCES)
			SKIP(return, "BPF stack trace requires privilege: %s",
			     strerror(saved_errno));
		errno = saved_errno;
		ASSERT_GE(prog_fd, 0) TH_LOG("%s", verifier_log);
	}

	link_fd = attach_sys_enter(prog_fd);
	if (link_fd < 0) {
		saved_errno = errno;
		close(prog_fd);
		close(stack_fd);
		munmap(code, PROCESS_PAGE_SIZE);
		close(elf_fd);
		if (saved_errno == EPERM || saved_errno == EACCES)
			SKIP(return, "BPF stack trace requires privilege: %s",
			     strerror(saved_errno));
		errno = saved_errno;
		ASSERT_GE(link_fd, 0);
	}

	for (attempt = 0; attempt < STACK_CAPTURE_ATTEMPTS; attempt++) {
		for (i = 0; i < TRIGGER_COUNT; i++)
			trigger();
		build_id_found = find_test_build_id(stack_fd, &reported_offset,
						    &stack_count);
		if (build_id_found)
			break;
	}
	close(link_fd);
	ksft_print_msg("captured stacks=%u reported offset=%#llx\n",
		       stack_count, (unsigned long long)reported_offset);
	EXPECT_GT(stack_count, 0U);
	EXPECT_TRUE(build_id_found);

	offset_ok = build_id_found && reported_offset >= CODE_FILE_OFFSET &&
		    reported_offset < CODE_FILE_OFFSET + PROCESS_PAGE_SIZE;
	EXPECT_TRUE(offset_ok);

	close(prog_fd);
	close(stack_fd);
	munmap(code, PROCESS_PAGE_SIZE);
	close(elf_fd);
}

P3S_HARNESS_4KB_MAIN
