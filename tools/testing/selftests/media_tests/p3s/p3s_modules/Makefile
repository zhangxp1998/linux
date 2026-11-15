# SPDX-License-Identifier: GPL-2.0
# Video buffer mapping fixtures for P3S selftests.
ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(srctree)/mm

include $(src)/../../../p3s_fixture_exports.mk

ifneq ($(CONFIG_VIDEOBUF2_VMALLOC),)
$(eval $(call p3s_export_fixture,vb2_mmap_p3s_module,vb2_core_expbuf vb2_core_queue_init vb2_core_queue_release vb2_core_reqbufs))
$(eval $(call p3s_export_fixture,vb2_userptr_reject_p3s_module,vb2_core_queue_init vb2_core_queue_release vb2_core_reqbufs))
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
