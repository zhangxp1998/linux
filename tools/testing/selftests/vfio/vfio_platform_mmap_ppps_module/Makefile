VFIO_PLATFORM_MMAP_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += vfio_platform_mmap_ppps_test.o
ccflags-y += -I$(srctree)/drivers/vfio/platform

all:
	+$(MAKE) -C $(KDIR) M=$(VFIO_PLATFORM_MMAP_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(VFIO_PLATFORM_MMAP_PPPS_MODULE_DIR) clean
