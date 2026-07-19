DRM_GEM_MMAP_PPPS_MODULE_DIR := \
	$(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(if $(O),$(O),$(realpath ../../../../../..))

obj-m += drm_gem_mmap_ppps_test.o

all:
	+$(MAKE) -C $(KDIR) M=$(DRM_GEM_MMAP_PPPS_MODULE_DIR) modules

clean:
	+$(MAKE) -C $(KDIR) M=$(DRM_GEM_MMAP_PPPS_MODULE_DIR) clean
