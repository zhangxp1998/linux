# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
TEST_GEN_FILES := dma_mmap_full_p3s dma_mmap_bounds_p3s iommu_dma_mmap_p3s
TEST_PROGS := iommu_dma_mmap_p3s.sh dma_mmap_full_attrs_p3s.sh \
	dma_mmap_full_pages_p3s.sh dma_mmap_bounds_p3s.sh \
	dma_mmap_bounds_native_p3s.sh
LOCAL_HDRS += dma_mmap_bounds_p3s.h
P3S_FIXTURE_MODULES := 1

include ../../p3s_build.mk
