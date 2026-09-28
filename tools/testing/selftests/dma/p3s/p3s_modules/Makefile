# SPDX-License-Identifier: GPL-2.0
# DMA mapping fixtures for P3S selftests.
ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(srctree)/mm

include $(src)/../../../p3s_fixture_exports.mk

obj-m += dma_mmap_attrs_p3s_module.o
obj-m += dma_mmap_pages_p3s_module.o
obj-m += dma_mmap_bounds_p3s_module.o
ifeq ($(filter-out $(p3s_exports),dma_alloc_noncontiguous dma_free_noncontiguous dma_mmap_noncontiguous dma_vmap_noncontiguous dma_vunmap_noncontiguous),)
CFLAGS_dma_mmap_bounds_p3s_module.o += -DP3S_DMA_NONCONTIG
endif
$(eval $(call p3s_export_fixture,iommu_dma_mmap_p3s_module,dma_mmap_noncontiguous))

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
