// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>

#define DRM_PPPS_CREATE 0x00

struct drm_ppps_create {
	__u64 offset;
	__u32 handle;
	__u32 pad;
};

#define DRM_IOCTL_PPPS_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PPPS_CREATE, struct drm_ppps_create)

static struct device *test_parent;
static struct drm_device *test_device;

struct test_object {
	struct drm_gem_object base;
	unsigned long buffer;
};

static const struct vm_operations_struct test_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static void test_object_free(struct drm_gem_object *obj)
{
	struct test_object *test_obj = container_of(obj, struct test_object,
						   base);

	free_pages(test_obj->buffer, get_order(obj->size));
	drm_gem_object_release(obj);
	kfree(test_obj);
}

static int test_object_mmap(struct drm_gem_object *obj,
			    struct vm_area_struct *vma)
{
	struct test_object *test_obj = container_of(obj, struct test_object,
						   base);

	vma->vm_ops = &test_vm_ops;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start,
			       virt_to_pfn((void *)test_obj->buffer),
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static const struct drm_gem_object_funcs test_object_funcs = {
	.free = test_object_free,
	.mmap = test_object_mmap,
};

static struct drm_gem_object *test_object_create(struct drm_device *dev)
{
	struct test_object *test_obj;
	int ret;

	test_obj = kzalloc_obj(*test_obj);
	if (!test_obj)
		return ERR_PTR(-ENOMEM);

	test_obj->buffer = __get_free_pages(GFP_KERNEL | __GFP_ZERO,
					    get_order(SZ_16K));
	if (!test_obj->buffer) {
		kfree(test_obj);
		return ERR_PTR(-ENOMEM);
	}

	drm_gem_private_object_init(dev, &test_obj->base, SZ_16K);
	test_obj->base.funcs = &test_object_funcs;
	ret = drm_gem_create_mmap_offset(&test_obj->base);
	if (ret) {
		drm_gem_object_put(&test_obj->base);
		return ERR_PTR(ret);
	}

	return &test_obj->base;
}

static int test_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct drm_ppps_create *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = test_object_create(dev);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, obj, &args->handle);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return ret;
}

static const struct drm_ioctl_desc test_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PPPS_CREATE, test_create_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(test_fops);

static const struct drm_driver test_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.ioctls = test_ioctls,
	.num_ioctls = ARRAY_SIZE(test_ioctls),
	.fops = &test_fops,
	.name = "drm_gem_mmap_ppps",
	.desc = "DRM GEM mmap PPPS regression fixture",
	.major = 1,
	.minor = 0,
};

static int __init test_init(void)
{
	int ret;

	test_parent = root_device_register("drm_gem_mmap_ppps_parent");
	if (IS_ERR(test_parent))
		return PTR_ERR(test_parent);

	test_device = drm_dev_alloc(&test_driver, test_parent);
	if (IS_ERR(test_device)) {
		ret = PTR_ERR(test_device);
		goto unregister_parent;
	}

	ret = drm_dev_register(test_device, 0);
	if (ret)
		goto put_device;

	return 0;

put_device:
	drm_dev_put(test_device);
unregister_parent:
	root_device_unregister(test_parent);
	return ret;
}

static void __exit test_exit(void)
{
	drm_dev_unregister(test_device);
	drm_dev_put(test_device);
	root_device_unregister(test_parent);
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DRM GEM mmap PPPS regression fixture");
