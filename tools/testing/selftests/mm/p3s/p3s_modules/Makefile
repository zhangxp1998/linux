# SPDX-License-Identifier: GPL-2.0
# Kernel-side fixtures for the mm P3S selftests, one misc device each.
ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(srctree)/mm

# Fixtures must use the exports of the tested kernel; do not add exports only
# for tests, and do not turn compiler failures into SKIPs.
include $(src)/../../../p3s_fixture_exports.mk

# mremap
obj-m += mremap_pgoff_p3s_module.o

# GUP and iov_iter
$(eval $(call p3s_export_fixture,gup_retry_p3s_module,pin_user_pages_unlocked))
obj-m += iov_iter_p3s_module.o
$(eval $(call p3s_export_fixture,fault_in_p3s_module,fault_in_readable fault_in_safe_writeable fault_in_writeable))
obj-m += vma_address_end_p3s_module.o
obj-m += mmap_fail_p3s_module.o
obj-m += iov_range_retry_p3s_module.o

# PFN and device mappings

# MMU notifier
ifneq ($(CONFIG_MMU_NOTIFIER),)
endif

else # userspace selftest build

KDIR ?= $(if $(O),$(O),/lib/modules/$(shell uname -r)/build)
P3S_KERNEL_SOURCE ?= $(if $(wildcard $(KDIR)/source),$(realpath $(KDIR)/source),$(KDIR))
p3s_module_cc = $(if $(LLVM),$(if $(filter 1,$(LLVM)),clang,$(if $(filter -%,$(LLVM)),clang$(LLVM),$(LLVM)clang)),$(CROSS_COMPILE)gcc)

all modules:
ifneq (,$(wildcard $(KDIR)/Module.symvers))
ifneq (,$(wildcard $(P3S_KERNEL_SOURCE)/include/linux/p3s.h))
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(p3s_module_cc) modules
else
	@echo "  SKIP $(notdir $(CURDIR)): tested kernel lacks P3S headers"
endif
else
	@echo "  SKIP $(notdir $(CURDIR)): no built kernel at $(KDIR)"
endif

clean:
ifneq (,$(wildcard $(KDIR)/Makefile))
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif

.PHONY: all modules clean
endif
