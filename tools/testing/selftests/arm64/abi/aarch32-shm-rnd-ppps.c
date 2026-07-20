// SPDX-License-Identifier: GPL-2.0
/* Regression test for AArch32 SHM_RND alignment on arm64 PPPS kernels. */

#define __NR_exit 1
#define __NR_write 4
#define __NR_munmap 91
#define __NR_mmap2 192
#define __NR_shmat 305
#define __NR_shmdt 306
#define __NR_shmget 307
#define __NR_shmctl 308

#define PROT_NONE 0
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_RMID 0
#define SHM_RND 020000
#define SHM_REMAP 040000

#define PROCESS_PAGE_SIZE 4096UL
#define COMPAT_SHMLBA (4 * PROCESS_PAGE_SIZE)
#define OLD_16K_SHMLBA (64 * 1024UL)
#define RESERVE_SIZE (4 * OLD_16K_SHMLBA)
#define TEST_VALUE 0x5a17c0deUL

static inline long syscall1(long number, long arg1)
{
	register long r0 __asm__("r0") = arg1;
	register long r7 __asm__("r7") = number;

	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
	return r0;
}

static inline long syscall3(long number, long arg1, long arg2, long arg3)
{
	register long r0 __asm__("r0") = arg1;
	register long r1 __asm__("r1") = arg2;
	register long r2 __asm__("r2") = arg3;
	register long r7 __asm__("r7") = number;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r7)
			 : "memory", "cc");
	return r0;
}

static inline long syscall6(long number, long arg1, long arg2, long arg3,
			    long arg4, long arg5, long arg6)
{
	register long r0 __asm__("r0") = arg1;
	register long r1 __asm__("r1") = arg2;
	register long r2 __asm__("r2") = arg3;
	register long r3 __asm__("r3") = arg4;
	register long r4 __asm__("r4") = arg5;
	register long r5 __asm__("r5") = arg6;
	register long r7 __asm__("r7") = number;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
			   "r"(r7)
			 : "memory", "cc");
	return r0;
}

static unsigned long string_length(const char *text)
{
	unsigned long length = 0;

	while (text[length])
		length++;
	return length;
}

static void print(const char *text)
{
	syscall3(__NR_write, 1, (long)text, string_length(text));
}

static int syscall_failed(long ret)
{
	return (unsigned long)ret >= (unsigned long)-4095;
}

static int test_shm_rnd(void)
{
	unsigned long base, target;
	long mapping = -1;
	long shmid = -1;
	int success = 0;

	base = syscall6(__NR_mmap2, 0, RESERVE_SIZE, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (syscall_failed(base))
		return 0;

	target = (base + OLD_16K_SHMLBA - 1) & ~(OLD_16K_SHMLBA - 1);
	target += COMPAT_SHMLBA;
	shmid = syscall3(__NR_shmget, IPC_PRIVATE, PROCESS_PAGE_SIZE,
			 IPC_CREAT | 0600);
	if (shmid < 0)
		goto out;
	mapping = syscall3(__NR_shmat, shmid, target, SHM_RND | SHM_REMAP);
	if (syscall_failed(mapping))
		goto out;
	*(unsigned long *)mapping = TEST_VALUE;
	success = (unsigned long)mapping == target &&
		  *(unsigned long *)mapping == TEST_VALUE;
out:
	if (!syscall_failed(mapping))
		syscall1(__NR_shmdt, mapping);
	if (shmid >= 0)
		syscall3(__NR_shmctl, shmid, IPC_RMID, 0);
	syscall3(__NR_munmap, base, RESERVE_SIZE, 0);
	return success;
}

void _start(void)
{
	print("TAP version 13\n1..1\n");
	if (test_shm_rnd()) {
		print("ok 1 - round AArch32 SysV shm at the ABI SHMLBA\n");
		syscall1(__NR_exit, 0);
	} else {
		print("not ok 1 - round AArch32 SysV shm at the ABI SHMLBA\n");
		syscall1(__NR_exit, 1);
	}
	__builtin_unreachable();
}
