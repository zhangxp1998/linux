# SPDX-License-Identifier: GPL-2.0

KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

TEST_GEN_PROGS := drm_gem_mmap_ppps ttm_vm_fault_ppps
TEST_PROGS := drm_mm.sh i915.sh test-drm-gem-mmap-ppps.sh \
	test-ttm-vm-fault-ppps.sh
DRM_GEM_MMAP_PPPS_MODULE := \
	drm_gem_mmap_ppps_module/drm_gem_mmap_ppps_test.ko
TTM_VM_FAULT_PPPS_MODULE := \
	ttm_vm_fault_ppps_module/ttm_vm_fault_ppps_test.ko
TEST_FILES := $(DRM_GEM_MMAP_PPPS_MODULE) $(TTM_VM_FAULT_PPPS_MODULE)

include ../../lib.mk

ifneq ($(DRM_GEM_MMAP_PPPS_MODULE),)
all: $(DRM_GEM_MMAP_PPPS_MODULE)

$(DRM_GEM_MMAP_PPPS_MODULE): \
	drm_gem_mmap_ppps_module/drm_gem_mmap_ppps_test.c \
	drm_gem_mmap_ppps_module/Makefile
	+$(MAKE) -C drm_gem_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_drm_gem_mmap_ppps_module
clean_drm_gem_mmap_ppps_module:
	+$(MAKE) -C drm_gem_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_drm_gem_mmap_ppps_module
endif

ifneq ($(TTM_VM_FAULT_PPPS_MODULE),)
all: $(TTM_VM_FAULT_PPPS_MODULE)

$(TTM_VM_FAULT_PPPS_MODULE): \
	ttm_vm_fault_ppps_module/ttm_vm_fault_ppps_test.c \
	ttm_vm_fault_ppps_module/Makefile
	+$(MAKE) -C ttm_vm_fault_ppps_module KDIR=$(KDIR)

.PHONY: clean_ttm_vm_fault_ppps_module
clean_ttm_vm_fault_ppps_module:
	+$(MAKE) -C ttm_vm_fault_ppps_module KDIR=$(KDIR) clean

clean: clean_ttm_vm_fault_ppps_module
endif
