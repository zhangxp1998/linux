// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/uio_driver.h>
#include <linux/vmalloc.h>

#define TEST_BYTES SZ_16K
#define TEST_SLICE_BYTES SZ_4K
#define NO_PHYSICAL_ADDRESS (~0UL)

static struct device *test_parent;
static void *test_buffer;
static void *test_map1;
static void *test_map2;
static unsigned long physical_addr = NO_PHYSICAL_ADDRESS;
static struct uio_info test_info;

module_param(physical_addr, ulong, 0444);
MODULE_PARM_DESC(physical_addr, "optional physical address for access testing");

static int __init test_init(void)
{
	static const u8 values[] = { 0x11, 0x22, 0x33, 0x44 };
	int ret;
	int i;

	test_buffer = vzalloc(TEST_BYTES);
	if (!test_buffer)
		return -ENOMEM;
	test_map1 = vzalloc(PAGE_SIZE);
	if (!test_map1) {
		ret = -ENOMEM;
		goto free_buffer;
	}
	test_map2 = vzalloc(PAGE_SIZE);
	if (!test_map2) {
		ret = -ENOMEM;
		goto free_map1;
	}
	for (i = 0; i < ARRAY_SIZE(values); i++)
		*((u8 *)test_buffer + i * TEST_SLICE_BYTES) = values[i];
	*((u8 *)test_map1) = 0x5a;
	*((u8 *)test_map2) = 0x6b;

	test_parent = root_device_register("uio_mmap_ppps_parent");
	if (IS_ERR(test_parent)) {
		ret = PTR_ERR(test_parent);
		goto free_map2;
	}

	test_info.name = "uio_mmap_ppps";
	test_info.version = "1";
	test_info.irq = UIO_IRQ_NONE;
	test_info.mem[0].name = "test_buffer";
	test_info.mem[0].addr = (uintptr_t)test_buffer;
	test_info.mem[0].size = TEST_BYTES;
	test_info.mem[0].memtype = UIO_MEM_VIRTUAL;
	test_info.mem[1].name = "map1";
	test_info.mem[1].addr = (uintptr_t)test_map1;
	test_info.mem[1].size = PAGE_SIZE;
	test_info.mem[1].memtype = UIO_MEM_VIRTUAL;
	test_info.mem[2].name = "map2";
	test_info.mem[2].addr = (uintptr_t)test_map2;
	test_info.mem[2].size = PAGE_SIZE;
	test_info.mem[2].memtype = UIO_MEM_VIRTUAL;

	if (physical_addr != NO_PHYSICAL_ADDRESS) {
		test_info.mem[3].name = "physical";
		test_info.mem[3].addr = physical_addr;
		test_info.mem[3].size = TEST_BYTES;
		test_info.mem[3].memtype = UIO_MEM_PHYS;
	}

	ret = uio_register_device(test_parent, &test_info);
	if (ret)
		goto unregister_parent;

	return 0;

unregister_parent:
	root_device_unregister(test_parent);
free_map2:
	vfree(test_map2);
free_map1:
	vfree(test_map1);
free_buffer:
	vfree(test_buffer);
	return ret;
}

static void __exit test_exit(void)
{
	uio_unregister_device(&test_info);
	root_device_unregister(test_parent);
	vfree(test_map2);
	vfree(test_map1);
	vfree(test_buffer);
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UIO mmap PPPS regression fixture");
