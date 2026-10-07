// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/mm_types.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/p3s/mm.h>
#include <linux/cacheflush.h>
#include "vma.h"
#include "p3s/pgtable.h"

enum p3s_4k_mode p3s_4k_mode __ro_after_init = P3S_4K_MODE_OFF;
EXPORT_SYMBOL(p3s_4k_mode);

static int __init parse_p3s_4k(char *str)
{
	bool enabled;

	if (!str)
		return -EINVAL;

	if (!kstrtobool(str, &enabled)) {
		p3s_4k_mode = enabled ? P3S_4K_MODE_ON : P3S_4K_MODE_OFF;
		return 0;
	}

	if (!strcmp(str, "alternate")) {
		p3s_4k_mode = P3S_4K_MODE_ALTERNATE;
		return 0;
	}

	return -EINVAL;
}
early_param("p3s_4k", parse_p3s_4k);

/* Map one explicit physical slice when it differs from the VMA phase. */
int p3s_remap_pfn_slice(struct vm_area_struct *vma, unsigned long addr,
			unsigned long pfn, unsigned int slice, pgprot_t prot)
{
	P3S_CONTEXT_REMOTE_MM(vma->vm_mm);
	struct mm_struct *mm = vma->vm_mm;
	unsigned long frame;
	pte_t *pte;
	spinlock_t *ptl;
	int err = 0;

	if (!mm_is_p3s_4k(mm) ||
	    !mm_pte_aligned(mm, addr) || addr < vma->vm_start ||
	    addr >= vma->vm_end || PAGE_SIZE_4KB > vma->vm_end - addr ||
	    slice >= mm_slices_per_page(mm))
		return -EINVAL;
	if (!pfn_modify_allowed(pfn, prot))
		return -EACCES;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	flush_cache_range(vma, addr, addr + PAGE_SIZE_4KB);
	pte = get_locked_pte(mm, addr, &ptl);
	if (!pte)
		return -ENOMEM;
	if (!pte_none(ptep_get(pte))) {
		err = -EBUSY;
		goto unlock;
	}
	frame = (pfn << P3S_SLICE_SHIFT) + slice;
	set_pte_at(mm, addr, pte, pte_mkspecial(mm_frame_pte(mm, frame, prot)));
unlock:
	pte_unmap_unlock(pte, ptl);
	return err;
}

#define vmg_is_p3s_4k(vmg)	mm_is_p3s_4k((vmg)->mm)

/*
 * vma_can_merge_offsets - Check that the range at @boundary continues @left
 * @left: Left (lower-address) VMA
 * @right_pgoff: Page offset of right (higher-address) range
 * @right_slice: Starting subpage slice of right range (0..3)
 * @boundary: Virtual address where left ends and right begins
 * @is_p3s_4k: Whether process is running in 4KB compat mode
 * @is_file: Whether mapping is file-backed
 */
static bool vma_can_merge_offsets(const struct vm_area_struct *left,
				  pgoff_t right_pgoff,
				  unsigned short right_slice,
				  unsigned long boundary,
				  bool is_p3s_4k, bool is_file)
{
	return vma_linear_page_index(left, boundary) == right_pgoff &&
	       (!is_p3s_4k || !is_file ||
		vma_slice_offset(left, boundary) == right_slice);
}

/*
 * vmg_new_range_slice - Slice at vmg->start of a proposed new range
 * @vmg: VMA merge structure without a middle VMA
 * @neighbour: The VMA to merge with, mapping the same file
 *
 * Private /dev/zero mappings keep vm_file without vm_ops and, like
 * anonymous memory, take their slice from the virtual address.
 */
static unsigned int vmg_new_range_slice(struct vma_merge_struct *vmg,
					struct vm_area_struct *neighbour)
{
	if (!neighbour->vm_ops)
		return (vmg->start >> PAGE_SHIFT_4KB) & P3S_SLICE_MASK;
	return vmg->slice_off;
}

/*
 * vmg_can_merge_offsets - Verify offset & slice continuity for VMA merge
 * @vmg: VMA merge structure
 * @merge_next: True if merging with vmg->next, false if merging with vmg->prev
 */
bool vmg_can_merge_offsets(struct vma_merge_struct *vmg, bool merge_next)
{
	if (merge_next) {
		unsigned int start_slice;
		unsigned long total_slices;

		if (!vmg_is_p3s_4k(vmg))
			return vmg->next->vm_pgoff ==
			       vmg->pgoff + PHYS_PFN(vmg->end - vmg->start);

		/*
		 * Count pages the way next does (see vma_linear_page_index()):
		 * 4KB pages if it is anonymous, else slices from vmg->slice_off.
		 */
		if (vma_is_anonymous(vmg->next))
			return vmg->next->vm_pgoff ==
			       vmg->pgoff + ((vmg->end - vmg->start) >> PAGE_SHIFT_4KB);

		start_slice = vmg_new_range_slice(vmg, vmg->next);
		total_slices = start_slice + ((vmg->end - vmg->start) >> PAGE_SHIFT_4KB);

		return vmg->next->vm_pgoff == vmg->pgoff + (total_slices >> P3S_SLICE_SHIFT) &&
		       vma_slice_offset(vmg->next, vmg->end) ==
		       (total_slices & P3S_SLICE_MASK);
	}

	if (!vmg->prev)
		return false;

	return vma_can_merge_offsets(vmg->prev, vmg->pgoff,
				     vmg->middle ? vma_slice_off(vmg->middle) :
				     vmg_new_range_slice(vmg, vmg->prev),
				     vmg->start, vmg_is_p3s_4k(vmg), !!vmg->file);
}

/*
 * p3s_remap_phys_range - Map physical memory into a 4KB compat VMA
 * @vma: 4KB compat VMA
 * @addr: First user address to map, 4KB aligned
 * @phys: Physical address to map at @addr, 4KB aligned
 * @size: Number of bytes to map, a multiple of 4KB
 * @prot: Page protection
 *
 * Each 4KB user page maps the next 4KB of physical memory, a slice of a
 * native page frame.  On failure, the pages mapped so far are zapped.
 */
static int p3s_remap_phys_range(struct vm_area_struct *vma, unsigned long addr,
				phys_addr_t phys, unsigned long size,
				pgprot_t prot)
{
	unsigned long off;
	int err = 0;

	/* The common case already follows the baseline's VMA slice phase. */
	if (((phys >> PAGE_SHIFT_4KB) & P3S_SLICE_MASK) ==
	    vma_slice_offset(vma, addr))
		return remap_pfn_range(vma, addr, phys >> PAGE_SHIFT_KERNEL,
				       size, prot);

	for (off = 0; off < size; off += PAGE_SIZE_4KB) {
		phys_addr_t pa = phys + off;

		err = p3s_remap_pfn_slice(vma, addr + off,
					  pa >> PAGE_SHIFT_KERNEL,
					  (pa >> PAGE_SHIFT_4KB) & P3S_SLICE_MASK,
					  prot);
		if (err)
			break;
	}
	if (err && off)
		zap_page_range_single(vma, addr, off, NULL);
	return err;
}

/*
 * p3s_vm_iomap_memory - vm_iomap_memory() for a 4KB compat VMA
 * @vma: 4KB compat VMA
 * @start: Physical start of the memory area
 * @len: Size of the memory area in bytes
 *
 * As in the native version, an unaligned @start maps from the page that
 * contains it, here a 4KB page.
 */
int p3s_vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start,
			unsigned long len)
{
	unsigned long vm_len = vma->vm_end - vma->vm_start;
	u64 offset = vma_file_offset(vma);
	u64 aligned_len;

	if (start + len < start)
		return -EINVAL;
	if (check_add_overflow(len, start & ~PAGE_MASK_4KB, &len))
		return -EINVAL;
	start &= PAGE_MASK_4KB;
	if (vma_aligned_size(vma, len, &aligned_len))
		return -EINVAL;
	if (offset > aligned_len || vm_len > aligned_len - offset)
		return -EINVAL;
	return p3s_remap_phys_range(vma, vma->vm_start, start + offset, vm_len,
				    pgprot_decrypted(vma->vm_page_prot));
}
