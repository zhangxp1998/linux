// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>

#include "../fault_in_ppps.h"

static long fault_in_ppps_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct fault_in_ppps_args request;
	void __user *address;

	if (cmd != FAULT_IN_PPPS_IOCTL)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.reserved)
		return -EINVAL;

	address = u64_to_user_ptr(request.address);
	switch (request.operation) {
	case FAULT_IN_PPPS_WRITEABLE:
		request.not_faulted = fault_in_writeable(address, request.length);
		break;
	case FAULT_IN_PPPS_SAFE_WRITEABLE:
		request.not_faulted = fault_in_safe_writeable(address, request.length);
		break;
	case FAULT_IN_PPPS_READABLE:
		request.not_faulted = fault_in_readable(address, request.length);
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations fault_in_ppps_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fault_in_ppps_ioctl,
	.compat_ioctl = fault_in_ppps_ioctl,
};

static struct miscdevice fault_in_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = FAULT_IN_PPPS_DEVICE_NAME,
	.fops = &fault_in_ppps_fops,
	.mode = 0600,
};

module_misc_device(fault_in_ppps_device);

MODULE_DESCRIPTION("PPPS fault-in helper regression test");
MODULE_LICENSE("GPL");
