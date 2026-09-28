# SPDX-License-Identifier: GPL-2.0
# DMA mapping fixtures for P3S selftests.
ifneq ($(KERNELRELEASE),)

include $(src)/../../../p3s_fixture_exports.mk

obj-m += dma_mmap_attrs_p3s_module.o
obj-m += dma_mmap_pages_p3s_module.o
obj-m += dma_mmap_bounds_p3s_module.o
ifeq ($(filter-out $(p3s_exports),dma_alloc_noncontiguous dma_free_noncontiguous dma_mmap_noncontiguous dma_vmap_noncontiguous dma_vunmap_noncontiguous),)
CFLAGS_dma_mmap_bounds_p3s_module.o += -DP3S_DMA_NONCONTIG
endif
$(eval $(call p3s_export_fixture,iommu_dma_mmap_p3s_module,dma_alloc_noncontiguous dma_free_noncontiguous dma_mmap_noncontiguous dma_vmap_noncontiguous dma_vunmap_noncontiguous))

else # userspace selftest build

include ../../../p3s_fixture_modules.mk
endif
