// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "../folio_within_range_ppps.h"
#include "../../../../../mm/internal.h"

#include "../../ppps/ppps_misc_module.h"

#define DEVICE_NAME "folio_within_range_ppps"
#define SUBPAGE_OFFSET SZ_4K

static const struct vm_operations_struct fixture_vm_ops;

static bool fixture_folio_within_range(unsigned long file_offset)
{
	struct folio folio = {};
	struct vm_area_struct vma = {
		.vm_mm = current->mm,
		.vm_start = SZ_1M,
		.vm_end = SZ_1M + PAGE_SIZE,
		.vm_pgoff = file_offset >> PAGE_SHIFT,
		.vm_ops = &fixture_vm_ops,
	};

	vma_set_slice_off(&vma, (file_offset >> MM_PAGE_SHIFT(current->mm)) &
				 PPPS_SLICE_MASK);
	folio.index = vma.vm_pgoff;
	return folio_within_range(&folio, &vma, vma.vm_start, vma.vm_end);
}

static long fixture_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	bool expected;
	bool within;

	switch (cmd) {
	case FOLIO_WITHIN_RANGE_PPPS_CHECK_ALIGNED:
		within = fixture_folio_within_range(0);
		expected = true;
		break;
	case FOLIO_WITHIN_RANGE_PPPS_CHECK_SUBPAGE:
		within = fixture_folio_within_range(SUBPAGE_OFFSET);
		expected = IS_ALIGNED(SUBPAGE_OFFSET, PAGE_SIZE);
		break;
	default:
		return -ENOTTY;
	}

	return within == expected ? 0 : -ERANGE;
}

static const struct file_operations fixture_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fixture_ioctl,
};

PPPS_MISC_MODULE(DEVICE_NAME, &fixture_fops, 0666,
		 ppps_misc_no_setup, ppps_misc_no_teardown,
		 "folio_within_range PPPS regression fixture");
