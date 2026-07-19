// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/io_uring.h>
#include <linux/io_uring_types.h>
#include <asm/shmparam.h>

#include "io_uring.h"
#include "memmap.h"
#include "kbuf.h"

static void *io_mem_alloc_compound(struct page **pages, int nr_pages,
				   size_t size, gfp_t gfp)
{
	struct page *page;
	int i, order;

	order = get_order(size);
	if (order > MAX_PAGE_ORDER)
		return ERR_PTR(-ENOMEM);
	else if (order)
		gfp |= __GFP_COMP;

	page = alloc_pages(gfp, order);
	if (!page)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < nr_pages; i++)
		pages[i] = page + i;

	return page_address(page);
}

static void *io_mem_alloc_single(struct page **pages, int nr_pages, size_t size,
				 gfp_t gfp)
{
	void *ret;
	int i;

	for (i = 0; i < nr_pages; i++) {
		pages[i] = alloc_page(gfp);
		if (!pages[i])
			goto err;
	}

	ret = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (ret)
		return ret;
err:
	while (i--)
		put_page(pages[i]);
	return ERR_PTR(-ENOMEM);
}

void *io_pages_map(struct page ***out_pages, unsigned short *npages,
		   size_t size)
{
	gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN;
	struct page **pages;
	int nr_pages;
	void *ret;

	nr_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	pages = kvmalloc_array(nr_pages, sizeof(struct page *), gfp);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	ret = io_mem_alloc_compound(pages, nr_pages, size, gfp);
	if (!IS_ERR(ret))
		goto done;
	if (nr_pages == 1)
		goto fail;

	ret = io_mem_alloc_single(pages, nr_pages, size, gfp);
	if (!IS_ERR(ret)) {
done:
		*out_pages = pages;
		*npages = nr_pages;
		return ret;
	}
fail:
	kvfree(pages);
	*out_pages = NULL;
	*npages = 0;
	return ret;
}

void io_pages_unmap(void *ptr, struct page ***pages, unsigned short *npages,
		    bool put_pages)
{
	bool do_vunmap = false;

	if (!ptr)
		return;

	if (put_pages && *npages) {
		struct page **to_free = *pages;
		int i;

		/*
		 * Only did vmap for the non-compound multiple page case.
		 * For the compound page, we just need to put the head.
		 */
		if (PageCompound(to_free[0]))
			*npages = 1;
		else if (*npages > 1)
			do_vunmap = true;
		for (i = 0; i < *npages; i++)
			put_page(to_free[i]);
	}
	if (do_vunmap)
		vunmap(ptr);
	kvfree(*pages);
	*pages = NULL;
	*npages = 0;
}

void io_pages_free(struct page ***pages, int npages)
{
	struct page **page_array = *pages;

	if (!page_array)
		return;

	unpin_user_pages(page_array, npages);
	kvfree(page_array);
	*pages = NULL;
}

struct page **io_pin_pages(unsigned long uaddr, unsigned long len, int *npages)
{
	unsigned long start, end, nr_pages;
	struct page **pages;
	int ret;

	if (check_add_overflow(uaddr, len, &end))
		return ERR_PTR(-EOVERFLOW);
	if (check_add_overflow(end, PAGE_SIZE - 1, &end))
		return ERR_PTR(-EOVERFLOW);

	end = end >> PAGE_SHIFT;
	start = uaddr >> PAGE_SHIFT;
	nr_pages = end - start;
	if (WARN_ON_ONCE(!nr_pages))
		return ERR_PTR(-EINVAL);

	pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	ret = pin_user_pages_fast(uaddr, nr_pages, FOLL_WRITE | FOLL_LONGTERM,
					pages);
	/* success, mapped all pages */
	if (ret == nr_pages) {
		*npages = nr_pages;
		return pages;
	}

	/* partial map, or didn't map anything */
	if (ret >= 0) {
		/* if we did partial map, release any pages we did get */
		if (ret)
			unpin_user_pages(pages, ret);
		ret = -EFAULT;
	}
	kvfree(pages);
	return ERR_PTR(ret);
}

void *__io_uaddr_map(struct page ***pages, unsigned short *npages,
		     unsigned long uaddr, size_t size, void **map_base)
{
	struct mm_struct *mm = current->mm;
	struct page **page_array;
	unsigned int page_offset = 0;
	unsigned int nr_pages;
	void *page_addr;
	int ret;

	*npages = 0;
	*map_base = NULL;
	uaddr = untagged_addr(uaddr);
	size = MM_PAGE_ALIGN(mm, size);

	if (!MM_PAGE_ALIGNED(mm, uaddr) || !size)
		return ERR_PTR(-EINVAL);

	if (ppps_mm_is_compat(mm)) {
		struct vm_area_struct *vma;

		/*
		 * Older io_uring stores these regions as a contiguous native
		 * vmap. Support one compat process page, matching the limitation
		 * of the newer registered-memory implementation.
		 */
		if (size != MM_PAGE_SIZE(mm))
			return ERR_PTR(-EOPNOTSUPP);

		page_array = kvmalloc_array(1, sizeof(*page_array),
					    GFP_KERNEL_ACCOUNT);
		if (!page_array)
			return ERR_PTR(-ENOMEM);

		mmap_read_lock(mm);
		vma = vma_lookup(mm, uaddr);
		if (!vma || size > vma->vm_end - uaddr) {
			ret = -EFAULT;
		} else {
			page_offset = vma_address_to_slice(vma, uaddr) *
				      MM_PAGE_SIZE(mm);
			ret = pin_user_pages(uaddr, 1,
					     FOLL_WRITE | FOLL_LONGTERM,
					     page_array);
		}
		mmap_read_unlock(mm);
		if (ret != 1) {
			if (ret > 0)
				unpin_user_pages(page_array, ret);
			kvfree(page_array);
			return ERR_PTR(ret < 0 ? ret : -EFAULT);
		}
		nr_pages = 1;
	} else {
		nr_pages = 0;
		page_array = io_pin_pages(uaddr, size, &nr_pages);
		if (IS_ERR(page_array))
			return page_array;
	}

	page_addr = vmap(page_array, nr_pages, VM_MAP, PAGE_KERNEL);
	if (page_addr) {
		*pages = page_array;
		*npages = nr_pages;
		*map_base = page_addr;
		return page_addr + page_offset;
	}

	io_pages_free(&page_array, nr_pages);
	return ERR_PTR(-ENOMEM);
}

static void *io_uring_validate_mmap_request(struct file *file, loff_t offset)
{
	struct io_ring_ctx *ctx = file->private_data;

	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		/* Don't allow mmap if the ring was setup without it */
		if (ctx->flags & IORING_SETUP_NO_MMAP)
			return ERR_PTR(-EINVAL);
		return ctx->rings;
	case IORING_OFF_SQES:
		/* Don't allow mmap if the ring was setup without it */
		if (ctx->flags & IORING_SETUP_NO_MMAP)
			return ERR_PTR(-EINVAL);
		return ctx->sq_sqes;
	case IORING_OFF_PBUF_RING: {
		struct io_buffer_list *bl;
		unsigned int bgid;
		void *ptr;

		bgid = (offset & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_PBUF_SHIFT;
		bl = io_pbuf_get_bl(ctx, bgid);
		if (IS_ERR(bl))
			return bl;
		ptr = bl->buf_ring;
		io_put_bl(ctx, bl);
		return ptr;
		}
	}

	return ERR_PTR(-EINVAL);
}

int io_uring_mmap_pages(struct io_ring_ctx *ctx, struct vm_area_struct *vma,
			struct page **pages, int npages, size_t mmap_size)
{
	size_t allowed_size = MM_PAGE_ALIGN(vma->vm_mm, mmap_size);
	unsigned long nr_pages = npages;

	if (allowed_size < mmap_size ||
	    vma->vm_end - vma->vm_start > allowed_size)
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND);
	return vm_insert_pages(vma, vma->vm_start, pages, &nr_pages);
}

#ifdef CONFIG_MMU

__cold int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ring_ctx *ctx = file->private_data;
	size_t sz = vma->vm_end - vma->vm_start;
	loff_t offset = vma_file_offset(vma);
	unsigned int npages;
	size_t mmap_size;
	void *ptr;

	ptr = io_uring_validate_mmap_request(file, offset);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		mmap_size = io_uring_mmap_size(ctx, offset);
		npages = min(ctx->n_ring_pages, (sz + PAGE_SIZE - 1) >> PAGE_SHIFT);
		return io_uring_mmap_pages(ctx, vma, ctx->ring_pages, npages,
					  mmap_size);
	case IORING_OFF_SQES:
		mmap_size = io_uring_mmap_size(ctx, offset);
		return io_uring_mmap_pages(ctx, vma, ctx->sqe_pages,
					   ctx->n_sqe_pages, mmap_size);
	case IORING_OFF_PBUF_RING:
		return io_pbuf_mmap(file, vma);
	}

	return -EINVAL;
}

unsigned long io_uring_get_unmapped_area(struct file *filp, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	loff_t offset = (loff_t)pgoff << MM_PAGE_SHIFT(current->mm);
	void *ptr;

	/*
	 * Do not allow to map to user-provided address to avoid breaking the
	 * aliasing rules. Userspace is not able to guess the offset address of
	 * kernel kmalloc()ed memory area.
	 */
	if (addr)
		return -EINVAL;

	ptr = io_uring_validate_mmap_request(filp, offset);
	if (IS_ERR(ptr))
		return -ENOMEM;

	/*
	 * Some architectures have strong cache aliasing requirements.
	 * For such architectures we need a coherent mapping which aliases
	 * kernel memory *and* userspace memory. To achieve that:
	 * - use a NULL file pointer to reference physical memory, and
	 * - use the kernel virtual address of the shared io_uring context
	 *   (instead of the userspace-provided address, which has to be 0UL
	 *   anyway).
	 * - use the same pgoff which the get_unmapped_area() uses to
	 *   calculate the page colouring.
	 * For architectures without such aliasing requirements, the
	 * architecture will return any suitable mapping because addr is 0.
	 */
	filp = NULL;
	flags |= MAP_SHARED;
	pgoff = 0;	/* has been translated to ptr above */
#ifdef SHM_COLOUR
	addr = (uintptr_t) ptr;
	pgoff = addr >> PAGE_SHIFT;
#else
	addr = 0UL;
#endif
	return mm_get_unmapped_area(current->mm, filp, addr, len, pgoff, flags);
}

#else /* !CONFIG_MMU */

int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	return is_nommu_shared_mapping(vma->vm_flags) ? 0 : -EINVAL;
}

unsigned int io_uring_nommu_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_DIRECT | NOMMU_MAP_READ | NOMMU_MAP_WRITE;
}

unsigned long io_uring_get_unmapped_area(struct file *file, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	loff_t offset = (loff_t)pgoff << MM_PAGE_SHIFT(current->mm);
	void *ptr;

	ptr = io_uring_validate_mmap_request(file, offset);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	return (unsigned long) ptr;
}

#endif /* !CONFIG_MMU */
