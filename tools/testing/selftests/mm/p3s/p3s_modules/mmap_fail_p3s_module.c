// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../p3s_misc_module.h"

static int mmap_fail(struct file *file, struct vm_area_struct *vma)
{
	return -EIO;
}

static const struct file_operations mmap_fail_fops = {
	.owner = THIS_MODULE,
	.mmap = mmap_fail,
};

P3S_MISC_MODULE("mmap_fail_p3s", &mmap_fail_fops, 0600,
		p3s_misc_no_setup, p3s_misc_no_teardown,
		"P3S failed mmap rollback fixture");
