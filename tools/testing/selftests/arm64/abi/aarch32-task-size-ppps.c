// SPDX-License-Identifier: GPL-2.0

#define __NR_exit 1
#define __NR_write 4
#define __NR_munmap 91
#define __NR_mmap2 192

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_ANONYMOUS 0x20
#define PAGE_SIZE_4K 4096

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

static int test_address(unsigned long address)
{
	long mapping;

	mapping = syscall6(__NR_mmap2, address, PAGE_SIZE_4K,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	if ((unsigned long)mapping != address)
		return 0;
	*(unsigned long *)address = 0x12345678;
	syscall3(__NR_munmap, address, PAGE_SIZE_4K, 0);
	return 1;
}

void _start(void)
{
	int failures = 0;

	print("TAP version 13\n1..3\n");
	if (test_address(0xffffc000UL)) {
		print("ok 1 - map first 4K page above the 16K task limit\n");
	} else {
		print("not ok 1 - map first 4K page above the 16K task limit\n");
		failures++;
	}
	if (test_address(0xffffd000UL)) {
		print("ok 2 - map second 4K page above the 16K task limit\n");
	} else {
		print("not ok 2 - map second 4K page above the 16K task limit\n");
		failures++;
	}
	if (test_address(0xffffe000UL)) {
		print("ok 3 - map third 4K page above the 16K task limit\n");
	} else {
		print("not ok 3 - map third 4K page above the 16K task limit\n");
		failures++;
	}

	syscall1(__NR_exit, failures != 0);
	__builtin_unreachable();
}
