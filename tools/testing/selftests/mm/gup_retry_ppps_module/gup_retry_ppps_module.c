// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/ppps.h>
#include <linux/uaccess.h>

#include "../gup_retry_ppps.h"

static long gup_retry_ppps_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct gup_retry_ppps_args request;
	struct page *expected = NULL;
	struct page *pages[2];
	long expected_count = 0;
	long count;

	if (cmd != GUP_RETRY_PPPS_IOCTL)
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	count = pin_user_pages_unlocked(request.address, ARRAY_SIZE(pages),
					pages, 0);
	request.nr_pinned = count;
	request.second_matches_expected = 0;
	request.reserved = 0;
	if (count == ARRAY_SIZE(pages)) {
		expected_count = pin_user_pages_unlocked(request.address +
				MM_PAGE_SIZE(current->mm), 1, &expected, 0);
		if (expected_count == 1)
			request.second_matches_expected = pages[1] == expected;
	}

	if (expected_count == 1)
		unpin_user_page(expected);
	if (count > 0)
		unpin_user_pages(pages, count);
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations gup_retry_ppps_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gup_retry_ppps_ioctl,
	.compat_ioctl = gup_retry_ppps_ioctl,
};

static struct miscdevice gup_retry_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = GUP_RETRY_PPPS_DEVICE_NAME,
	.fops = &gup_retry_ppps_fops,
};

static int __init gup_retry_ppps_init(void)
{
	return misc_register(&gup_retry_ppps_device);
}

static void __exit gup_retry_ppps_exit(void)
{
	misc_deregister(&gup_retry_ppps_device);
}

module_init(gup_retry_ppps_init);
module_exit(gup_retry_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS GUP retry regression test helper");
