// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-resv.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_resource.h>

#include "../../../ppps/ppps_misc_module.h"

#define DEVICE_NAME "ttm_vm_fault_ppps"
#define TEST_MAP_SIZE SZ_16K
#define TEST_PAGE_COUNT 4

struct ttm_vm_fault_fixture {
	struct ttm_buffer_object bo;
	struct ttm_device bdev;
	struct ttm_device_funcs funcs;
	struct ttm_resource_manager manager;
	struct ttm_resource resource;
	struct dma_resv resv;
	unsigned long backing;
};

static struct ttm_vm_fault_fixture fixture;

static unsigned long fixture_io_mem_pfn(struct ttm_buffer_object *bo,
					unsigned long page_offset)
{
	return virt_to_pfn((void *)fixture.backing) + page_offset;
}

static vm_fault_t fixture_fault(struct vm_fault *vmf)
{
	vm_fault_t ret;

	ret = ttm_bo_vm_reserve(&fixture.bo, vmf);
	if (ret)
		return ret;

	ret = ttm_bo_vm_fault_reserved(vmf, vmf->vma->vm_page_prot,
				       TTM_BO_VM_NUM_PREFAULT);
	dma_resv_unlock(fixture.bo.base.resv);
	return ret;
}

static const struct vm_operations_struct fixture_vm_ops = {
	.fault = fixture_fault,
};

static int fixture_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != TEST_MAP_SIZE)
		return -EINVAL;

	vma->vm_private_data = &fixture.bo;
	vma->vm_ops = &fixture_vm_ops;
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}

static const struct file_operations fixture_fops = {
	.owner = THIS_MODULE,
	.mmap = fixture_mmap,
};

static int fixture_setup(void)
{
	fixture.backing = __get_free_pages(GFP_KERNEL | __GFP_ZERO,
					   get_order(TEST_PAGE_COUNT * PAGE_SIZE));
	if (!fixture.backing)
		return -ENOMEM;

	memcpy((void *)fixture.backing, "TTM-PPPS", 8);
	dma_resv_init(&fixture.resv);
	fixture.bo.base.resv = &fixture.resv;
	fixture.bo.base.size = TEST_PAGE_COUNT * PAGE_SIZE;
	fixture.bo.bdev = &fixture.bdev;
	fixture.bo.resource = &fixture.resource;
	fixture.bdev.funcs = &fixture.funcs;
	fixture.bdev.man_drv[0] = &fixture.manager;
	fixture.funcs.io_mem_pfn = fixture_io_mem_pfn;
	fixture.manager.use_tt = false;
	fixture.resource.mem_type = 0;
	fixture.resource.size = TEST_PAGE_COUNT * PAGE_SIZE;
	fixture.resource.bus.offset = virt_to_phys((void *)fixture.backing);
	fixture.resource.bus.is_iomem = true;
	fixture.resource.bus.caching = ttm_cached;
	return 0;
}

static void fixture_teardown(void)
{
	dma_resv_fini(&fixture.resv);
	free_pages(fixture.backing, get_order(TEST_PAGE_COUNT * PAGE_SIZE));
}

PPPS_MISC_MODULE(DEVICE_NAME, &fixture_fops, 0666, fixture_setup,
		 fixture_teardown, "TTM fault PPPS regression fixture");
