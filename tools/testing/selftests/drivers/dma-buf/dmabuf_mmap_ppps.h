/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DMABUF_MMAP_PPPS_H
#define DMABUF_MMAP_PPPS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DMABUF_VMAP_PPPS_POINTS 2

struct dmabuf_vmap_ppps {
	__s32 fd;
	__u32 count;
	__u64 offsets[DMABUF_VMAP_PPPS_POINTS];
	__u8 expected[DMABUF_VMAP_PPPS_POINTS];
	__u8 replacement[DMABUF_VMAP_PPPS_POINTS];
};

#define DMABUF_VMAP_PPPS_CHECK _IOW('D', 0x70, struct dmabuf_vmap_ppps)

#endif
