PAGEWALK_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += pagewalk_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(PAGEWALK_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(PAGEWALK_PPPS_MODULE_DIR) clean
