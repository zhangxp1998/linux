// SPDX-License-Identifier: GPL-2.0
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "vfio_platform_private.h"

#define DEVICE_NAME "vfio_platform_mmap_ppps"
#define TEST_REGION_SIZE SZ_16K

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

static struct miscdevice fixture_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &fixture_fops,
	.mode = 0666,
};

static int __init fixture_init(void)
{
	int ret;

	fixture.backing = (unsigned long)alloc_pages_exact(TEST_REGION_SIZE,
							GFP_KERNEL | __GFP_ZERO);
	if (!fixture.backing)
		return -ENOMEM;
	*(unsigned char *)fixture.backing = 0x11;
	*(unsigned char *)(fixture.backing + SZ_4K) = 0x22;

	fixture.region.addr = virt_to_phys((void *)fixture.backing);
	fixture.region.size = TEST_REGION_SIZE;
	fixture.region.flags = VFIO_REGION_INFO_FLAG_READ |
		VFIO_REGION_INFO_FLAG_WRITE | VFIO_REGION_INFO_FLAG_MMAP;
	fixture.region.type = VFIO_PLATFORM_REGION_TYPE_MMIO;
	fixture.vdev.regions = &fixture.region;
	fixture.vdev.num_regions = 1;

	ret = misc_register(&fixture_miscdev);
	if (ret)
		free_pages_exact((void *)fixture.backing, TEST_REGION_SIZE);
	return ret;
}

static void __exit fixture_exit(void)
{
	misc_deregister(&fixture_miscdev);
	free_pages_exact((void *)fixture.backing, TEST_REGION_SIZE);
}

module_init(fixture_init);
module_exit(fixture_exit);

MODULE_DESCRIPTION("VFIO platform mmap PPPS regression fixture");
MODULE_LICENSE("GPL");
