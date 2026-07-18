# SPDX-License-Identifier: GPL-2.0

VM_MAP_PAGES_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += vm_map_pages_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(VM_MAP_PAGES_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(VM_MAP_PAGES_PPPS_MODULE_DIR) clean
