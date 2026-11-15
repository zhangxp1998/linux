// SPDX-License-Identifier: GPL-2.0

#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../../../mm/p3s/p3s_misc_module.h"

#define TEST_PLANE_SIZE 4096
#define VB2_USERPTR_REJECT_P3S_REQBUFS _IO('v', 0x71)
#define VB2_USERPTR_REJECT_P3S_CREATE_BUFS _IO('v', 0x72)

static DEFINE_MUTEX(test_queue_lock);

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

static long test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vb2_queue q = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.io_modes = VB2_USERPTR,
		.ops = &test_queue_ops,
		.mem_ops = &vb2_vmalloc_memops,
		.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
		.lock = &test_queue_lock,
	};
	int ret;

	if (cmd != VB2_USERPTR_REJECT_P3S_REQBUFS &&
	    cmd != VB2_USERPTR_REJECT_P3S_CREATE_BUFS)
		return -ENOTTY;

	ret = vb2_queue_init(&q);
	if (ret)
		return ret;

	if (cmd == VB2_USERPTR_REJECT_P3S_REQBUFS) {
		struct v4l2_requestbuffers req = {
			.count = 1,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_USERPTR,
		};

		ret = vb2_reqbufs(&q, &req);
	} else {
		struct v4l2_create_buffers create = {
			.count = 1,
			.memory = V4L2_MEMORY_USERPTR,
			.format = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				.fmt.pix.sizeimage = TEST_PLANE_SIZE,
			},
		};

		ret = vb2_create_bufs(&q, &create);
	}

	vb2_queue_release(&q);
	return ret;
}

static const struct file_operations test_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = test_ioctl,
};

P3S_MISC_MODULE("vb2_userptr_reject_p3s", &test_fops, 0600,
		p3s_misc_no_setup, p3s_misc_no_teardown,
		"videobuf2 USERPTR rejection regression fixture");
