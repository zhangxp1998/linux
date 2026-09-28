# SPDX-License-Identifier: GPL-2.0
# Shared userspace build rules for P3S kernel fixture modules.

KDIR ?= $(if $(O),$(O),/lib/modules/$(shell uname -r)/build)
P3S_KERNEL_SOURCE ?= $(if $(wildcard $(KDIR)/source),$(realpath $(KDIR)/source),$(KDIR))
p3s_module_cc = $(if $(LLVM),$(if $(filter 1,$(LLVM)),clang,$(if $(filter -%,$(LLVM)),clang$(LLVM),$(LLVM)clang)),$(CROSS_COMPILE)gcc)

all modules:
ifneq (,$(wildcard $(KDIR)/Module.symvers))
ifneq (,$(wildcard $(P3S_KERNEL_SOURCE)/include/linux/p3s.h))
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(p3s_module_cc) modules
else
	@echo "  SKIP $(notdir $(CURDIR)): tested kernel lacks P3S headers"
endif
else
	@echo "  SKIP $(notdir $(CURDIR)): no built kernel at $(KDIR)"
endif

clean:
ifneq (,$(wildcard $(KDIR)/Makefile))
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif

.PHONY: all modules clean
