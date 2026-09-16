// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "../dmabuf_mmap_ppps.h"
#include "../../../ppps/ppps_misc_module.h"

#define TEST_BUFFER_SIZE (4 * 4096UL)

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
	/* Exported dma-buf VMAs may not be merged with adjacent VMAs. */
	vm_flags_set(vma, VM_DONTEXPAND);
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
	return dma_buf_mmap(file->private_data, vma, 0);
}

static long test_device_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct dmabuf_vmap_ppps request;
	struct iosys_map map = {};
	struct dma_buf *dmabuf;
	unsigned int i;
	long ret;

	if (cmd != DMABUF_VMAP_PPPS_CHECK ||
	    copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EINVAL;
	if (!request.count || request.count > DMABUF_VMAP_PPPS_POINTS)
		return -EINVAL;
	dmabuf = dma_buf_get(request.fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	for (i = 0; i < request.count; i++)
		if (request.offsets[i] >= dmabuf->size) {
			ret = -EINVAL;
			goto put;
		}
	ret = dma_buf_vmap(dmabuf, &map);
	if (ret)
		goto put;
	for (i = 0; i < request.count; i++) {
		u8 actual;

		iosys_map_memcpy_from(&actual, &map, request.offsets[i], 1);
		if (actual != request.expected[i]) {
			ret = -EUCLEAN;
			goto unmap;
		}
		iosys_map_memcpy_to(&map, request.offsets[i],
				    &request.replacement[i], 1);
	}
unmap:
	dma_buf_vunmap(dmabuf, &map);
put:
	dma_buf_put(dmabuf);
	return ret;
}

static int test_device_open(struct inode *inode, struct file *file)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;

	exp_info.ops = &test_dmabuf_ops;
	exp_info.size = TEST_BUFFER_SIZE;
	exp_info.flags = O_RDWR;
	exp_info.priv = &test_private;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	file->private_data = dmabuf;
	return 0;
}

static int test_device_release(struct inode *inode, struct file *file)
{
	dma_buf_put(file->private_data);
	return 0;
}

static const struct file_operations test_fops = {
	.owner = THIS_MODULE,
	.open = test_device_open,
	.release = test_device_release,
	.unlocked_ioctl = test_device_ioctl,
	.mmap = test_device_mmap,
};

PPPS_MISC_MODULE("dmabuf_mmap_ppps", &test_fops, 0,
		 ppps_misc_no_setup, ppps_misc_no_teardown,
		 "Test dma_buf_mmap() with per-process page sizes");
MODULE_IMPORT_NS("DMA_BUF");
