/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IOV_ITER_PPPS_H
#define IOV_ITER_PPPS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IOV_ITER_PPPS_DEVICE_NAME "iov_iter_ppps"

struct iov_iter_ppps_args {
	__u64 address;
	__u32 length;
	__u32 native_page_size;
	__s32 get_pages_result;
	__s32 extract_pages_result;
	__s32 bulk_first_len;
	__u32 reserved;
};

#define IOV_ITER_PPPS_IOCTL \
	_IOWR('i', 0x71, struct iov_iter_ppps_args)

#endif
