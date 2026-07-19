FOLIO_WITHIN_RANGE_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

obj-m += folio_within_range_ppps_test.o

all:
	+$(MAKE) -C $(KDIR) M=$(FOLIO_WITHIN_RANGE_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(FOLIO_WITHIN_RANGE_PPPS_MODULE_DIR) clean
