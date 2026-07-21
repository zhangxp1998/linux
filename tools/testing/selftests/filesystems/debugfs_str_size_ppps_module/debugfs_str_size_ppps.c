// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>

static struct dentry *test_dir;
static char *test_value;

static int __init debugfs_str_size_ppps_init(void)
{
	test_value = kstrdup("", GFP_KERNEL);
	if (!test_value)
		return -ENOMEM;
	test_dir = debugfs_create_dir("ppps_debugfs_str", NULL);
	if (IS_ERR(test_dir)) {
		kfree(test_value);
		return PTR_ERR(test_dir);
	}
	debugfs_create_str("value", 0600, test_dir, &test_value);
	return 0;
}

static void __exit debugfs_str_size_ppps_exit(void)
{
	debugfs_remove_recursive(test_dir);
	kfree(test_value);
}

module_init(debugfs_str_size_ppps_init);
module_exit(debugfs_str_size_ppps_exit);

MODULE_DESCRIPTION("PPPS debugfs string write regression fixture");
MODULE_LICENSE("GPL");
