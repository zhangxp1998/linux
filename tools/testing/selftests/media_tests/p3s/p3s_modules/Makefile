# SPDX-License-Identifier: GPL-2.0
# Video buffer mapping fixtures for P3S selftests.
ifneq ($(KERNELRELEASE),)

include $(src)/../../../p3s_fixture_exports.mk

ifneq ($(CONFIG_VIDEOBUF2_VMALLOC),)
$(eval $(call p3s_export_fixture,vb2_mmap_p3s_module,vb2_mmap vb2_queue_init vb2_queue_release vb2_reqbufs vb2_vmalloc_memops))
$(eval $(call p3s_export_fixture,vb2_userptr_reject_p3s_module,vb2_create_bufs vb2_queue_init vb2_queue_release vb2_reqbufs vb2_vmalloc_memops))
endif

else # userspace selftest build

include ../../../p3s_fixture_modules.mk
endif
