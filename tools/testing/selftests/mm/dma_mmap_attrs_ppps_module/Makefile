DMA_MMAP_ATTRS_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += dma_mmap_attrs_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(DMA_MMAP_ATTRS_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(DMA_MMAP_ATTRS_PPPS_MODULE_DIR) clean
