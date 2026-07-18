SIMPLE_IOMAP_PREPARE_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += simple_ioremap_prepare_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(SIMPLE_IOMAP_PREPARE_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(SIMPLE_IOMAP_PREPARE_PPPS_MODULE_DIR) clean
