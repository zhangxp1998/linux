// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/module.h>
#include <linux/vfio.h>

struct ppps_vfio_device {
	struct vfio_device vdev;
};

static struct device *ppps_device;
static struct ppps_vfio_device *ppps_vdev;

static const struct vfio_device_ops ppps_vfio_ops = {
	.name = "ppps-vfio-test",
};

static int __init ppps_vfio_init(void)
{
	int ret;

	ppps_device = root_device_register("ppps-vfio-test");
	if (IS_ERR(ppps_device))
		return PTR_ERR(ppps_device);

	ppps_vdev = vfio_alloc_device(ppps_vfio_device, vdev, ppps_device,
				      &ppps_vfio_ops);
	if (IS_ERR(ppps_vdev)) {
		ret = PTR_ERR(ppps_vdev);
		goto err_device;
	}

	ret = vfio_register_emulated_iommu_dev(&ppps_vdev->vdev);
	if (ret)
		goto err_vfio;
	return 0;

err_vfio:
	vfio_put_device(&ppps_vdev->vdev);
err_device:
	root_device_unregister(ppps_device);
	return ret;
}

static void __exit ppps_vfio_exit(void)
{
	vfio_unregister_group_dev(&ppps_vdev->vdev);
	vfio_put_device(&ppps_vdev->vdev);
	root_device_unregister(ppps_device);
}

module_init(ppps_vfio_init);
module_exit(ppps_vfio_exit);

MODULE_DESCRIPTION("VFIO group device-name PPPS regression fixture");
MODULE_LICENSE("GPL");
