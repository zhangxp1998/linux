// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif
#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif
#ifndef KVM_VM_TYPE_ARM_PROTECTED
#define KVM_VM_TYPE_ARM_PROTECTED BIT(31)
#endif

#define USER_PAGE_SIZE 4096UL
#define GUEST_CODE_GPA 0x40000000ULL
#define GUEST_MMIO_GPA 0x10000000ULL

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

static int run_test(void)
{
	const uint32_t ldr_x0_x1 = 0xf9400020;
	struct kvm_userspace_memory_region region = {
		.slot = 0,
		.guest_phys_addr = GUEST_CODE_GPA,
	};
	struct kvm_vcpu_init init = {};
	struct rlimit limit;
	struct kvm_run *run;
	unsigned long host_page_size;
	unsigned long vcpu_mmap_size;
	unsigned long guest_map_size;
	uintptr_t guest_addr;
	void *guest_map;
	void *lock_map;
	int vcpu_fd = -1;
	int vm_fd = -1;
	int kvm_fd = -1;
	int ret = 1;

	kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm_fd < 0) {
		perror("open /dev/kvm");
		goto out;
	}

	vcpu_mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if ((long)vcpu_mmap_size <= 0) {
		perror("KVM_GET_VCPU_MMAP_SIZE");
		goto out;
	}
	host_page_size = vcpu_mmap_size /
		(KVM_COALESCED_MMIO_PAGE_OFFSET + 1);
	if (host_page_size < USER_PAGE_SIZE ||
	    host_page_size % USER_PAGE_SIZE) {
		fprintf(stderr, "invalid KVM host page size: %lu\n",
			host_page_size);
		goto out;
	}
	if (geteuid()) {
		printf("SKIP: test must run as root to drop all capabilities\n");
		ret = KSFT_SKIP;
		goto out;
	}

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, KVM_VM_TYPE_ARM_PROTECTED);
	if (vm_fd < 0) {
		if (errno == EINVAL || errno == ENODEV) {
			printf("SKIP: protected KVM is unavailable\n");
			ret = KSFT_SKIP;
			goto out;
		}
		perror("KVM_CREATE_VM");
		goto out;
	}
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) {
		perror("KVM_CREATE_VCPU");
		goto out;
	}
	if (ioctl(vm_fd, KVM_ARM_PREFERRED_TARGET, &init) ||
	    ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init)) {
		perror("KVM_ARM_VCPU_INIT");
		goto out;
	}

	guest_map_size = 2 * host_page_size;
	guest_map = mmap(NULL, guest_map_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guest_map == MAP_FAILED) {
		perror("mmap guest memory");
		goto out;
	}
	guest_addr = ((uintptr_t)guest_map + host_page_size - 1) &
		     ~(host_page_size - 1);
	memcpy((void *)guest_addr, &ldr_x0_x1, sizeof(ldr_x0_x1));

	region.memory_size = host_page_size;
	region.userspace_addr = guest_addr;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region)) {
		perror("KVM_SET_USER_MEMORY_REGION");
		goto out_guest;
	}

	if (set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc)),
			GUEST_CODE_GPA) ||
	    set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[1])),
			GUEST_MMIO_GPA)) {
		perror("KVM_SET_ONE_REG");
		goto out_guest;
	}

	run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vcpu_fd, 0);
	if (run == MAP_FAILED) {
		perror("mmap kvm_run");
		goto out_guest;
	}

	limit.rlim_cur = host_page_size + USER_PAGE_SIZE;
	limit.rlim_max = limit.rlim_cur;
	if (setrlimit(RLIMIT_MEMLOCK, &limit)) {
		perror("setrlimit");
		goto out_run;
	}
	if (setuid(65534)) {
		perror("setuid");
		goto out_run;
	}

	lock_map = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (lock_map == MAP_FAILED) {
		perror("mmap lock page");
		goto out_run;
	}
	if (mlock(lock_map, USER_PAGE_SIZE)) {
		perror("mlock 4K page");
		goto out_lock;
	}

	errno = 0;
	if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
		fprintf(stderr,
			"KVM_RUN failed with %s (host page %lu, memlock %llu)\n",
			strerror(errno), host_page_size,
			(unsigned long long)limit.rlim_cur);
		goto out_unlock;
	}
	if (run->exit_reason != KVM_EXIT_MMIO) {
		fprintf(stderr, "unexpected KVM exit reason: %u\n",
			run->exit_reason);
		goto out_unlock;
	}

	printf("PASS: pKVM pin: host_page=%lu memlock_limit=%llu\n",
	       host_page_size,
	       (unsigned long long)limit.rlim_cur);
	ret = 0;

out_unlock:
	munlock(lock_map, USER_PAGE_SIZE);
out_lock:
	munmap(lock_map, USER_PAGE_SIZE);
out_run:
	munmap(run, vcpu_mmap_size);
out_guest:
	munmap(guest_map, guest_map_size);
out:
	if (vcpu_fd >= 0)
		close(vcpu_fd);
	if (vm_fd >= 0)
		close(vm_fd);
	if (kvm_fd >= 0)
		close(kvm_fd);
	return ret;
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0) {
		perror("personality get");
		return 1;
	}
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0) {
		perror("personality set");
		return 1;
	}
	execl("/proc/self/exe", "kvm_pkvm_memlock_ppps", "--run", NULL);
	perror("exec");
	return 1;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return 1;
}
