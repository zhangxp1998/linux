// SPDX-License-Identifier: GPL-2.0
/*
 * Exercise the real DMA-GEM import helper with initialized attachment/SG
 * metadata. No DMA is submitted; the fixture retains ownership of its SG.
 */
#include <drm/drm_drv.h>
#include <drm/drm_gem_dma_helper.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>

#include "../drm_gem_dma_import_ppps.h"
#include "../../../ppps/ppps_misc_module.h"

static struct device *parent;
static struct drm_device *drm;

static long fixture_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dma_buf dmabuf = { .size = PAGE_SIZE };
	struct dma_buf_attachment attach = { .dmabuf = &dmabuf };
	struct scatterlist sg[2];
	struct sg_table sgt = { .sgl = sg, .nents = 1, .orig_nents = 1 };
	struct drm_gem_object *obj;
	bool accept = arg == DRM_DMA_ALIGNED || arg == DRM_DMA_CONTIGUOUS_SG;
	int ret;

	if (cmd != DRM_DMA_IMPORT_PPPS_CHECK)
		return -ENOTTY;
	if (arg >= DRM_DMA_CASES)
		return -EINVAL;

	sg_init_table(sg, ARRAY_SIZE(sg));
	sg_dma_address(&sg[0]) = 4 * PAGE_SIZE;
	sg_dma_len(&sg[0]) = PAGE_SIZE;
	sg_dma_address(&sg[1]) = 5 * PAGE_SIZE;
	sg_dma_len(&sg[1]) = PAGE_SIZE;
	switch (arg) {
	case DRM_DMA_SHORT_SIZE:
		dmabuf.size = SZ_4K;
		break;
	case DRM_DMA_UNALIGNED_ADDRESS:
		sg_dma_address(&sg[0]) += SZ_4K;
		break;
	case DRM_DMA_SHORT_SG:
		sg_dma_len(&sg[0]) = PAGE_SIZE / 2;
		break;
	case DRM_DMA_CONTIGUOUS_SG:
	case DRM_DMA_NONCONTIGUOUS_SG:
		dmabuf.size = 2 * PAGE_SIZE;
		sgt.nents = sgt.orig_nents = 2;
		if (arg == DRM_DMA_NONCONTIGUOUS_SG)
			sg_dma_address(&sg[1]) += PAGE_SIZE;
		break;
	}

	obj = drm_gem_dma_prime_import_sg_table(drm, &attach, &sgt);
	if (IS_ERR(obj))
		return !accept && PTR_ERR(obj) == -EINVAL ? 0 : PTR_ERR(obj);
	ret = accept && obj->size == dmabuf.size &&
		to_drm_gem_dma_obj(obj)->dma_addr == sg_dma_address(&sg[0]) ?
		0 : -EINVAL;
	/* The direct helper has not transferred attach/SG ownership to GEM. */
	drm_gem_object_put(obj);
	return ret;
}

static const struct file_operations fixture_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fixture_ioctl,
};

static const struct drm_driver fixture_driver = {
	.driver_features = DRIVER_GEM,
	.name = "drm_dma_import_ppps",
	.desc = "DMA-GEM import boundary test",
	.major = 1,
};

static int fixture_setup(void)
{
	int ret;

	if (PAGE_SIZE != SZ_16K)
		return -EOPNOTSUPP;
	parent = root_device_register("drm_dma_import_ppps_parent");
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	drm = drm_dev_alloc(&fixture_driver, parent);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		root_device_unregister(parent);
		return ret;
	}
	return 0;
}

static void fixture_teardown(void)
{
	drm_dev_put(drm);
	root_device_unregister(parent);
}

PPPS_MISC_MODULE("drm_dma_import_ppps", &fixture_fops, 0600,
		 fixture_setup, fixture_teardown,
		 "DMA-GEM native alignment and SG extent regression tests");
