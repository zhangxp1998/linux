# SPDX-License-Identifier: GPL-2.0
FAULT_IN_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += fault_in_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(FAULT_IN_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(FAULT_IN_PPPS_MODULE_DIR) clean
