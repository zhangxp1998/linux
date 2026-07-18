MMAP_ACTION_MAP_KERNEL_PAGES_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += mmap_action_map_kernel_pages_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(MMAP_ACTION_MAP_KERNEL_PAGES_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(MMAP_ACTION_MAP_KERNEL_PAGES_PPPS_MODULE_DIR) clean
