VFIO_GROUP_NAME_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += vfio_group_name_ppps_test.o

all:
	+$(MAKE) -C $(KDIR) M=$(VFIO_GROUP_NAME_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(VFIO_GROUP_NAME_PPPS_MODULE_DIR) clean
