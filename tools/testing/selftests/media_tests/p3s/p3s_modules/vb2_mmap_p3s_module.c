// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../../../mm/p3s/p3s_misc_module.h"

#define TEST_PLANE_SIZE (6 * SZ_1K)

static DEFINE_MUTEX(test_queue_lock);
static struct vb2_queue test_queue;

static int test_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			    unsigned int *num_planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	if (*num_planes)
		return sizes[0] < TEST_PLANE_SIZE ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = TEST_PLANE_SIZE;
	return 0;
}

static void test_buf_queue(struct vb2_buffer *vb)
{
}

static const struct vb2_ops test_queue_ops = {
	.queue_setup = test_queue_setup,
	.buf_queue = test_buf_queue,
};

static int test_mmap(struct file *file, struct vm_area_struct *vma)
{
	return vb2_mmap(&test_queue, vma);
}

static const struct file_operations test_fops = {
	.owner = THIS_MODULE,
	.mmap = test_mmap,
};

static int test_setup(void)
{
	struct v4l2_requestbuffers req = {
		.count = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	int ret;

	test_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	test_queue.io_modes = VB2_MMAP;
	test_queue.ops = &test_queue_ops;
	test_queue.mem_ops = &vb2_vmalloc_memops;
	test_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	test_queue.lock = &test_queue_lock;

	ret = vb2_queue_init(&test_queue);
	if (ret)
		return ret;
	ret = vb2_reqbufs(&test_queue, &req);
	if (ret)
		vb2_queue_release(&test_queue);
	return ret;
}

static void test_teardown(void)
{
	vb2_queue_release(&test_queue);
}

P3S_MISC_MODULE("vb2_mmap_p3s", &test_fops, 0600, test_setup, test_teardown,
		"videobuf2 mmap P3S regression fixture");
