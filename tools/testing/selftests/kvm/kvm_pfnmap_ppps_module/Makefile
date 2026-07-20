# SPDX-License-Identifier: GPL-2.0

KVM_PFNMAP_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += kvm_pfnmap_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(KVM_PFNMAP_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(KVM_PFNMAP_PPPS_MODULE_DIR) clean
