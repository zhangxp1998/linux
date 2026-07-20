// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

#define USER_PAGE_SIZE 4096UL
#define NATIVE_16K_SIZE 16384UL
#define PFNMAP_SIZE (32UL * 1024 * 1024)
#define PFNMAP_GPA 0x80000000ULL
#define GUEST_CODE_GPA 0x40000000ULL
#define GUEST_MMIO_GPA 0x10000000ULL
#define TEST_MAGIC UINT64_C(0x4b564d50464e4d50)

static int set_one_reg(int vcpu_fd, uint64_t id, uint64_t value)
{
	struct kvm_one_reg reg = {
		.id = id,
		.addr = (uintptr_t)&value,
	};

	return ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg);
}

static uint64_t core_reg_id(uint64_t offset)
{
	return KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | offset;
}

static bool setup_guest(int vm_fd, int vcpu_fd, void *pfnmap,
			void **guest_reservation)
{
	const __u32 guest_code[] = {
		0xf9400023, /* ldr x3, [x1] */
		0xf9000043, /* str x3, [x2] */
	};
	struct kvm_userspace_memory_region pfnmap_region = {
		.slot = 1,
		.guest_phys_addr = PFNMAP_GPA,
		.memory_size = PFNMAP_SIZE,
		.userspace_addr = (uintptr_t)pfnmap,
	};
	struct kvm_userspace_memory_region code_region = {
		.slot = 0,
		.guest_phys_addr = GUEST_CODE_GPA,
		.memory_size = NATIVE_16K_SIZE,
	};
	struct kvm_vcpu_init init = {};
	uintptr_t code_addr;

	if (ioctl(vm_fd, KVM_ARM_PREFERRED_TARGET, &init) ||
	    ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init))
		return false;

	*guest_reservation = mmap(NULL, 2 * NATIVE_16K_SIZE,
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*guest_reservation == MAP_FAILED)
		return false;
	code_addr = ((uintptr_t)*guest_reservation + NATIVE_16K_SIZE - 1) &
		    ~(NATIVE_16K_SIZE - 1);
	memcpy((void *)code_addr, guest_code, sizeof(guest_code));
	code_region.userspace_addr = code_addr;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &code_region) ||
	    ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &pfnmap_region))
		return false;

	return !set_one_reg(vcpu_fd,
		core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc)), GUEST_CODE_GPA) &&
	       !set_one_reg(vcpu_fd,
		core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[1])),
		PFNMAP_GPA + NATIVE_16K_SIZE) &&
	       !set_one_reg(vcpu_fd,
		core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[2])),
		GUEST_MMIO_GPA);
}

static bool run_guest(int kvm_fd, int vcpu_fd)
{
	struct kvm_run *run;
	unsigned long mmap_size;
	__u64 value = 0;
	bool success;

	mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (!mmap_size)
		return false;
	run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vcpu_fd, 0);
	if (run == MAP_FAILED)
		return false;

	success = ioctl(vcpu_fd, KVM_RUN, 0) == 0 &&
		  run->exit_reason == KVM_EXIT_MMIO && run->mmio.is_write &&
		  run->mmio.phys_addr == GUEST_MMIO_GPA &&
		  run->mmio.len == sizeof(value);
	if (success) {
		memcpy(&value, run->mmio.data, sizeof(value));
		success = value == TEST_MAGIC;
	}
	munmap(run, mmap_size);
	return success;
}

static int run_test(void)
{
	void *guest_reservation = MAP_FAILED;
	void *reservation = MAP_FAILED;
	void *mapping = MAP_FAILED;
	uintptr_t target = 0;
	bool guest_ready = false;
	bool guest_ok = false;
	int vcpu_fd = -1;
	int vm_fd = -1;
	int kvm_fd = -1;
	int fixture_fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fixture_fd = open("/dev/kvm-pfnmap-ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fixture_fd >= 0, "open the PFNMAP fixture\n");
	if (fixture_fd < 0)
		goto out;

	reservation = mmap(NULL, 3 * PFNMAP_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation != MAP_FAILED) {
		target = ((uintptr_t)reservation + PFNMAP_SIZE - 1) &
			 ~(PFNMAP_SIZE - 1);
		mapping = mmap((void *)target, PFNMAP_SIZE,
			       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
			       fixture_fd, USER_PAGE_SIZE);
	}
	ksft_test_result(mapping != MAP_FAILED && mapping == (void *)target,
			 "map a block-aligned PFNMAP at a 4K file offset\n");
	ksft_test_result(mapping != MAP_FAILED &&
			 __atomic_load_n((__u64 *)mapping, __ATOMIC_RELAXED) == 0,
			 "fault the PFNMAP fixture in userspace\n");
	if (mapping == MAP_FAILED)
		goto out;

	kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	ret = kvm_fd < 0 ? -1 : ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
	ksft_test_result(ret == KVM_API_VERSION, "open the arm64 KVM API\n");
	if (ret != KVM_API_VERSION)
		goto out;

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	vcpu_fd = vm_fd < 0 ? -1 : ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	ksft_test_result(vm_fd >= 0 && vcpu_fd >= 0,
			 "create a VM and VCPU\n");
	if (vcpu_fd < 0)
		goto out;

	guest_ready = setup_guest(vm_fd, vcpu_fd, mapping,
				  &guest_reservation);
	ksft_test_result(guest_ready,
			 "install anonymous and PFNMAP guest memory\n");
	if (guest_ready)
		guest_ok = run_guest(kvm_fd, vcpu_fd);
	ksft_test_result(guest_ok,
			 "guest reads the faulted PFNMAP process page\n");
out:
	if (vcpu_fd >= 0)
		close(vcpu_fd);
	if (vm_fd >= 0)
		close(vm_fd);
	if (kvm_fd >= 0)
		close(kvm_fd);
	if (guest_reservation != MAP_FAILED)
		munmap(guest_reservation, 2 * NATIVE_16K_SIZE);
	if (reservation != MAP_FAILED)
		munmap(reservation, 3 * PFNMAP_SIZE);
	if (fixture_fd >= 0)
		close(fixture_fd);
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
	execl("/proc/self/exe", "kvm_pfnmap_ppps", "--run", NULL);
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
