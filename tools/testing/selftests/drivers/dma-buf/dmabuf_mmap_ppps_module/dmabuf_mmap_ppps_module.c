// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#define TEST_BUFFER_SIZE (4 * 4096UL)

static struct dma_buf *test_dmabuf;
static int test_private;

static struct sg_table *test_map_dma_buf(struct dma_buf_attachment *attachment,
					 enum dma_data_direction direction)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static void test_unmap_dma_buf(struct dma_buf_attachment *attachment,
			       struct sg_table *table,
			       enum dma_data_direction direction)
{
}

static void test_release(struct dma_buf *dmabuf)
{
}

static int test_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	return vma_file_offset(vma) ? -EINVAL : 0;
}

static const struct dma_buf_ops test_dmabuf_ops = {
	.map_dma_buf = test_map_dma_buf,
	.unmap_dma_buf = test_unmap_dma_buf,
	.release = test_release,
	.mmap = test_mmap,
};

static int test_device_mmap(struct file *file, struct vm_area_struct *vma)
{
	return dma_buf_mmap(test_dmabuf, vma, 0);
}

static const struct file_operations test_fops = {
	.owner = THIS_MODULE,
	.mmap = test_device_mmap,
};

static struct miscdevice test_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dmabuf_mmap_ppps",
	.fops = &test_fops,
};

static int __init test_init(void)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int error;

	exp_info.ops = &test_dmabuf_ops;
	exp_info.size = TEST_BUFFER_SIZE;
	exp_info.flags = O_RDWR;
	exp_info.priv = &test_private;
	test_dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(test_dmabuf))
		return PTR_ERR(test_dmabuf);

	error = misc_register(&test_misc);
	if (error)
		dma_buf_put(test_dmabuf);
	return error;
}

static void __exit test_exit(void)
{
	misc_deregister(&test_misc);
	dma_buf_put(test_dmabuf);
}

module_init(test_init);
module_exit(test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test dma_buf_mmap() with per-process page sizes");
MODULE_IMPORT_NS("DMA_BUF");
