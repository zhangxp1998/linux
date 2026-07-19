TTM_VM_FAULT_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../../..))

obj-m += ttm_vm_fault_ppps_test.o

all:
	+$(MAKE) -C $(KDIR) M=$(TTM_VM_FAULT_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(TTM_VM_FAULT_PPPS_MODULE_DIR) clean
