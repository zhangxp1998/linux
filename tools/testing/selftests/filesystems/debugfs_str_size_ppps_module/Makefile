# SPDX-License-Identifier: GPL-2.0
MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += debugfs_str_size_ppps.o

all:
	+$(MAKE) -C $(KDIR) M=$(MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(MODULE_DIR) clean
