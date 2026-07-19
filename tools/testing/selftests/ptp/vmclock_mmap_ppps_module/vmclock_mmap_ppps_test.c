// SPDX-License-Identifier: GPL-2.0

#include <linux/gfp.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <uapi/linux/vmclock-abi.h>

static unsigned long test_page;
static struct platform_device *test_device;

static int __init test_init(void)
{
	struct vmclock_abi *clk;
	struct resource resource;

	test_page = get_zeroed_page(GFP_KERNEL);
	if (!test_page)
		return -ENOMEM;

	clk = (struct vmclock_abi *)test_page;
	clk->magic = cpu_to_le32(VMCLOCK_MAGIC);
	clk->size = cpu_to_le32(PAGE_SIZE);
	clk->version = cpu_to_le16(1);
	clk->counter_id = VMCLOCK_COUNTER_ARM_VCNT;
	clk->time_type = VMCLOCK_TIME_TAI;
	clk->clock_status = VMCLOCK_STATUS_FREERUNNING;

	resource.start = virt_to_phys((void *)test_page);
	resource.end = resource.start + PAGE_SIZE - 1;
	resource.flags = IORESOURCE_MEM;
	test_device = platform_device_register_resndata(NULL, "vmclock",
							PLATFORM_DEVID_AUTO,
							&resource, 1, NULL, 0);
	if (IS_ERR(test_device)) {
		free_page(test_page);
		return PTR_ERR(test_device);
	}

	return 0;
}

static void __exit test_exit(void)
{
	platform_device_unregister(test_device);
	free_page(test_page);
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vmclock mmap PPPS regression fixture");
