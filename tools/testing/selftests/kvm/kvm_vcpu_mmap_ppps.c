// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define MAX_NATIVE_PAGE_SIZE 16384UL
#define MAPPING_SIZE (2 * USER_PAGE_SIZE)
#define DIRTY_RING_SIZE 4096
#define GUEST_CODE_GPA 0x40000000ULL
#define GUEST_MMIO_GPA 0x10000000ULL
#define GUEST_EXIT_MMIO_GPA 0x20000000ULL
#define PAGEMAP_PRESENT (UINT64_C(1) << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define USER_COALESCED_MMIO_MAX \
	((USER_PAGE_SIZE - sizeof(struct kvm_coalesced_mmio_ring)) / \
	 sizeof(struct kvm_coalesced_mmio))

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

static bool mapping_is_accessible(void *mapping, size_t size)
{
	uint64_t sum;
	int pipefd[2];
	pid_t pid;
	int status;

	if (pipe(pipefd))
		return false;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}
	if (!pid) {
		size_t offset;

		close(pipefd[0]);
		sum = 0;
		for (offset = 0; offset < size; offset += USER_PAGE_SIZE)
			sum += *((unsigned char *)mapping + offset);
		_exit(write(pipefd[1], &sum, sizeof(sum)) == sizeof(sum) ? 0 : 1);
	}
	close(pipefd[1]);
	if (read(pipefd[0], &sum, sizeof(sum)) != sizeof(sum)) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return false;
	}
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid)
		return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int mapping_pfn(void *mapping, uint64_t *pfn)
{
	uint64_t entry;
	unsigned char byte;
	int pipefd[2];
	off_t offset;
	int fd;

	if (pipe(pipefd))
		return -errno;
	if (write(pipefd[1], mapping, sizeof(byte)) != sizeof(byte) ||
	    read(pipefd[0], &byte, sizeof(byte)) != sizeof(byte)) {
		int err = errno ? -errno : -EIO;

		close(pipefd[0]);
		close(pipefd[1]);
		return err;
	}
	close(pipefd[0]);
	close(pipefd[1]);
	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	offset = (uintptr_t)mapping / USER_PAGE_SIZE * sizeof(entry);
	if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry)) {
		int err = errno ? -errno : -EIO;

		close(fd);
		return err;
	}
	close(fd);
	if (!(entry & PAGEMAP_PRESENT))
		return -EFAULT;
	*pfn = entry & PAGEMAP_PFN_MASK;
	return 0;
}

static void test_coalesced_mmio(int vm_fd, int vcpu_fd, void *run_mapping,
				void *mmio_mapping)
{
	const uint32_t guest_code[] = {
		0xb9000020, /* str w0, [x1] */
		0xf9400040, /* ldr x0, [x2] */
	};
	struct kvm_userspace_memory_region region = {
		.slot = 0,
		.guest_phys_addr = GUEST_CODE_GPA,
		.memory_size = MAX_NATIVE_PAGE_SIZE,
	};
	struct kvm_coalesced_mmio_zone zone = {
		.addr = GUEST_MMIO_GPA,
		.size = sizeof(uint32_t),
	};
	struct kvm_coalesced_mmio_ring *ring = mmio_mapping;
	struct kvm_vcpu_init init = {};
	struct kvm_run *run = run_mapping;
	uintptr_t guest_addr = 0;
	void *guest_mapping;
	bool empty_ring_ok = false;
	bool full_ring_ok = false;
	bool region_ok = false;
	bool regs_ok = false;
	bool zone_ok = false;
	int pc_ret = -1;
	int run_ret = -1;
	int ret;

	ret = ioctl(vm_fd, KVM_ARM_PREFERRED_TARGET, &init);
	if (!ret)
		ret = ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init);
	ksft_test_result(ret == 0, "initialize an arm64 VCPU\n");

	guest_mapping = mmap(NULL, 2 * MAX_NATIVE_PAGE_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guest_mapping != MAP_FAILED) {
		guest_addr = ((uintptr_t)guest_mapping + MAX_NATIVE_PAGE_SIZE - 1) &
			     ~(MAX_NATIVE_PAGE_SIZE - 1);
		memcpy((void *)guest_addr, guest_code, sizeof(guest_code));
		region.userspace_addr = guest_addr;
		region_ok = ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) == 0;
	}
	ksft_test_result(region_ok, "install guest memory\n");

	if (!ret && region_ok) {
		regs_ok = !set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc)), GUEST_CODE_GPA) &&
			!set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[0])), 0x12345678) &&
			!set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[1])), GUEST_MMIO_GPA) &&
			!set_one_reg(vcpu_fd,
			core_reg_id(KVM_REG_ARM_CORE_REG(regs.regs[2])), GUEST_EXIT_MMIO_GPA);
	}
	ksft_test_result(regs_ok, "set guest MMIO registers\n");

	if (!ret && region_ok)
		zone_ok = ioctl(vm_fd, KVM_REGISTER_COALESCED_MMIO, &zone) == 0;
	ksft_test_result(zone_ok, "register a coalesced-MMIO zone\n");

	if (regs_ok && zone_ok && run_mapping != MAP_FAILED &&
	    mmio_mapping != MAP_FAILED) {
		ring->first = 0;
		ring->last = 0;
		ret = ioctl(vcpu_fd, KVM_RUN, 0);
		empty_ring_ok = ret == 0 && ring->last == 1 &&
			ring->coalesced_mmio[0].phys_addr == GUEST_MMIO_GPA &&
			ring->coalesced_mmio[0].len == sizeof(uint32_t);
	}
	ksft_test_result(empty_ring_ok,
			 "guest MMIO reaches the userspace coalesced ring\n");

	if (empty_ring_ok) {
		run->immediate_exit = 1;
		ioctl(vcpu_fd, KVM_RUN, 0);
		run->immediate_exit = 0;
		pc_ret = set_one_reg(vcpu_fd,
				     core_reg_id(KVM_REG_ARM_CORE_REG(regs.pc)),
				     GUEST_CODE_GPA);
	}
	if (empty_ring_ok && !pc_ret) {
		ring->first = 0;
		ring->last = USER_COALESCED_MMIO_MAX - 1;
		run_ret = ioctl(vcpu_fd, KVM_RUN, 0);
		full_ring_ok = run_ret == 0 && run->exit_reason == KVM_EXIT_MMIO &&
			run->mmio.phys_addr == GUEST_MMIO_GPA;
	}
	ksft_test_result(full_ring_ok,
			 "kernel honors the 4K coalesced-ring capacity\n");

	if (guest_mapping != MAP_FAILED)
		munmap(guest_mapping, 2 * MAX_NATIVE_PAGE_SIZE);
}

static int run_test(void)
{
	struct kvm_enable_cap cap = {
		.cap = KVM_CAP_DIRTY_LOG_RING_ACQ_REL,
		.args[0] = DIRTY_RING_SIZE,
	};
	unsigned long mmap_size;
	unsigned long start_pgoff;
	uint64_t run_pfn = 0, mmio_pfn = 0;
	void *dirty_mapping;
	void *mmio_mapping;
	void *run_mapping;
	void *mapping;
	int mmio_offset;
	int vcpu_fd;
	int vm_fd;
	int kvm_fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(24);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	ksft_test_result(kvm_fd >= 0, "open /dev/kvm\n");
	if (kvm_fd < 0)
		ksft_exit_fail_msg("open /dev/kvm failed: %s\n",
				   strerror(errno));

	ret = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
	ksft_test_result(ret == KVM_API_VERSION, "KVM API is available\n");
	if (ret != KVM_API_VERSION)
		ksft_exit_fail_msg("unexpected KVM API version: %d\n", ret);

	mmio_offset = ioctl(kvm_fd, KVM_CHECK_EXTENSION,
			    KVM_CAP_COALESCED_MMIO);
	ksft_test_result(mmio_offset == KVM_COALESCED_MMIO_PAGE_OFFSET,
			 "query the coalesced-MMIO page offset\n");

	mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	ksft_test_result(mmap_size >= MAPPING_SIZE,
			 "query the VCPU mmap size\n");
	ksft_test_result(mmap_size == MAPPING_SIZE,
			 "VCPU mmap size uses process-page units\n");

	ret = ioctl(kvm_fd, KVM_CHECK_EXTENSION,
		    KVM_CAP_DIRTY_LOG_RING_ACQ_REL);
	ksft_test_result(ret >= DIRTY_RING_SIZE,
			 "KVM dirty ring supports a 4K process-page ring\n");
	if (ret < DIRTY_RING_SIZE)
		ksft_exit_skip("KVM dirty ring is unavailable\n");

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd >= 0)
		ret = ioctl(vm_fd, KVM_ENABLE_CAP, &cap);
	else
		ret = -1;
	ksft_test_result(vm_fd >= 0 && ret == 0,
			 "create a VM with a dirty ring\n");
	if (vm_fd < 0 || ret)
		ksft_exit_fail_msg("dirty-ring VM creation failed: %s\n",
				   strerror(errno));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	ksft_test_result(vcpu_fd >= 0, "create a VCPU\n");
	if (vcpu_fd < 0)
		ksft_exit_fail_msg("VCPU creation failed: %s\n",
				   strerror(errno));

	start_pgoff = KVM_DIRTY_LOG_PAGE_OFFSET -
			(MAPPING_SIZE / USER_PAGE_SIZE);
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		       vcpu_fd, start_pgoff * USER_PAGE_SIZE);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a private VCPU range ending before the dirty ring\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);

	run_mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, vcpu_fd, 0);
	ksft_test_result(run_mapping != MAP_FAILED, "map the KVM run page\n");
	mmio_mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_SHARED, vcpu_fd,
			    mmio_offset * USER_PAGE_SIZE);
	ksft_test_result(mmio_mapping != MAP_FAILED,
			 "map the coalesced-MMIO page\n");
	ksft_test_result(run_mapping != MAP_FAILED &&
			 mapping_is_accessible(run_mapping, USER_PAGE_SIZE),
			 "KVM run page is accessible\n");
	ksft_test_result(mmio_mapping != MAP_FAILED &&
			 mapping_is_accessible(mmio_mapping, USER_PAGE_SIZE),
			 "coalesced-MMIO page is accessible\n");
	ret = run_mapping == MAP_FAILED ? -EINVAL :
		mapping_pfn(run_mapping, &run_pfn);
	ret |= mmio_mapping == MAP_FAILED ? -EINVAL :
		mapping_pfn(mmio_mapping, &mmio_pfn);
	ksft_test_result(ret == 0, "read both VCPU mapping PFNs\n");
	ksft_test_result(ret == 0 && run_pfn != mmio_pfn,
			 "coalesced-MMIO does not alias the KVM run page\n");

	dirty_mapping = mmap(NULL, DIRTY_RING_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, vcpu_fd,
			     KVM_DIRTY_LOG_PAGE_OFFSET * USER_PAGE_SIZE);
	ksft_test_result(dirty_mapping != MAP_FAILED, "map the dirty ring\n");
	ksft_test_result(dirty_mapping != MAP_FAILED &&
			 mapping_is_accessible(dirty_mapping, DIRTY_RING_SIZE),
			 "every 4K dirty-ring page is accessible\n");

	test_coalesced_mmio(vm_fd, vcpu_fd, run_mapping, mmio_mapping);

	if (dirty_mapping != MAP_FAILED)
		munmap(dirty_mapping, DIRTY_RING_SIZE);
	if (mmio_mapping != MAP_FAILED)
		munmap(mmio_mapping, USER_PAGE_SIZE);
	if (run_mapping != MAP_FAILED)
		munmap(run_mapping, USER_PAGE_SIZE);

	close(vcpu_fd);
	close(vm_fd);
	close(kvm_fd);
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
	execl("/proc/self/exe", "kvm_vcpu_mmap_ppps", "--run", NULL);
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
