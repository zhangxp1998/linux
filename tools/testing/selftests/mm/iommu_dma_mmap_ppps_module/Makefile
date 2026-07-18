# SPDX-License-Identifier: GPL-2.0

IOMMU_DMA_MMAP_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += iommu_dma_mmap_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(IOMMU_DMA_MMAP_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(IOMMU_DMA_MMAP_PPPS_MODULE_DIR) clean
