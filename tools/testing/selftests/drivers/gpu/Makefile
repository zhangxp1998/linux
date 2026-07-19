# SPDX-License-Identifier: GPL-2.0

KDIR ?= $(if $(O),$(O),$(realpath ../../../../..))

TEST_GEN_PROGS := drm_gem_mmap_ppps
TEST_PROGS := drm_mm.sh i915.sh test-drm-gem-mmap-ppps.sh
DRM_GEM_MMAP_PPPS_MODULE := \
	drm_gem_mmap_ppps_module/drm_gem_mmap_ppps_test.ko
TEST_FILES := $(DRM_GEM_MMAP_PPPS_MODULE)

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
