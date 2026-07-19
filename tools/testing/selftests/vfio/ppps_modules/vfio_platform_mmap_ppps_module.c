// SPDX-License-Identifier: GPL-2.0
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "vfio_platform_private.h"

#include "../../ppps/ppps_misc_module.h"

#define DEVICE_NAME "vfio_platform_mmap_ppps"

struct vfio_platform_mmap_fixture {
	struct vfio_platform_device vdev;
	struct vfio_platform_region region;
	unsigned long backing;
};

static struct vfio_platform_mmap_fixture fixture;

static int fixture_mmap(struct file *file, struct vm_area_struct *vma)
{
	return vfio_platform_mmap(&fixture.vdev.vdev, vma);
}

static const struct file_operations fixture_fops = {
	.owner = THIS_MODULE,
	.mmap = fixture_mmap,
};

static int fixture_setup(void)
{
	fixture.backing = (unsigned long)alloc_pages_exact(TEST_REGION_SIZE,
							GFP_KERNEL | __GFP_ZERO);
	if (!fixture.backing)
		return -ENOMEM;
	memcpy((void *)fixture.backing, "VFIO-PPPS", 9);

	fixture.region.addr = virt_to_phys((void *)fixture.backing);
	fixture.region.size = PAGE_SIZE;
	fixture.region.flags = VFIO_REGION_INFO_FLAG_READ |
		VFIO_REGION_INFO_FLAG_WRITE | VFIO_REGION_INFO_FLAG_MMAP;
	fixture.regions[0].type = VFIO_PLATFORM_REGION_TYPE_MMIO;
	fixture.regions[1] = fixture.regions[0];
	fixture.regions[1].size = SZ_4K;
	fixture.vdev.regions = fixture.regions;
	fixture.vdev.num_regions = ARRAY_SIZE(fixture.regions);
	return 0;
}

static void fixture_teardown(void)
{
	free_pages_exact((void *)fixture.backing, TEST_REGION_SIZE);
}

PPPS_MISC_MODULE(DEVICE_NAME, &fixture_fops, 0666, fixture_setup,
		 fixture_teardown, "VFIO platform mmap PPPS regression fixture");
