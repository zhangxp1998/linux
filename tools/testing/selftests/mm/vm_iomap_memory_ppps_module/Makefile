VM_IOMAP_MEMORY_PPPS_MODULE_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += vm_iomap_memory_ppps_module.o

all:
	+$(MAKE) -C $(KDIR) M=$(VM_IOMAP_MEMORY_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(VM_IOMAP_MEMORY_PPPS_MODULE_DIR) clean
