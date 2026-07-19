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
#include "rsrc.h"
#include "zcrx.h"

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
	if (WARN_ON_ONCE(nr_pages > INT_MAX))
		return ERR_PTR(-EOVERFLOW);

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

enum {
	/* memory was vmap'ed for the kernel, freeing the region vunmap's it */
	IO_REGION_F_VMAP			= 1,
	/* memory is provided by user and pinned by the kernel */
	IO_REGION_F_USER_PROVIDED		= 2,
	/* only the first page in the array is ref'ed */
	IO_REGION_F_SINGLE_REF			= 4,
};

void io_free_region(struct io_ring_ctx *ctx, struct io_mapped_region *mr)
{
	if (mr->pages) {
		long nr_refs = mr->nr_pages;

		if (mr->flags & IO_REGION_F_SINGLE_REF)
			nr_refs = 1;

		if (mr->flags & IO_REGION_F_USER_PROVIDED)
			unpin_user_pages(mr->pages, nr_refs);
		else
			release_pages(mr->pages, nr_refs);

		kvfree(mr->pages);
	}
	if ((mr->flags & IO_REGION_F_VMAP) && mr->ptr)
		vunmap(mr->ptr);
	if (mr->nr_pages && ctx->user)
		__io_unaccount_mem(ctx->user, mr->nr_pages);

	memset(mr, 0, sizeof(*mr));
}

static int io_region_init_ptr(struct io_mapped_region *mr)
{
	struct io_imu_folio_data ifd;
	void *ptr;

	if (io_check_coalesce_buffer(mr->pages, mr->nr_pages, &ifd)) {
		if (ifd.nr_folios == 1 && !PageHighMem(mr->pages[0])) {
			mr->ptr = page_address(mr->pages[0]);
			return 0;
		}
	}
	ptr = vmap(mr->pages, mr->nr_pages, VM_MAP, PAGE_KERNEL);
	if (!ptr)
		return -ENOMEM;

	mr->ptr = ptr;
	mr->flags |= IO_REGION_F_VMAP;
	return 0;
}

static int io_region_pin_compat_page(struct io_mapped_region *mr,
				     struct io_uring_region_desc *reg)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr = untagged_addr(reg->user_addr);
	struct vm_area_struct *vma;
	struct page **pages;
	long pinned;

	/*
	 * A native kernel mapping cannot concatenate arbitrary 4K slices from
	 * different native pages. A single process page has no such gap and is
	 * sufficient for registered wait arguments.
	 */
	if (reg->size != MM_PAGE_SIZE(mm))
		return -EOPNOTSUPP;

	pages = kvmalloc_array(1, sizeof(struct page *), GFP_KERNEL_ACCOUNT);
	if (!pages)
		return -ENOMEM;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (!vma || reg->size > vma->vm_end - addr) {
		pinned = -EFAULT;
	} else {
		mr->page_offset = vma_address_to_slice(vma, addr) *
				  MM_PAGE_SIZE(mm);
		pinned = pin_user_pages(addr, 1, FOLL_WRITE | FOLL_LONGTERM,
					pages);
	}
	mmap_read_unlock(mm);
	if (pinned != 1) {
		if (pinned > 0)
			unpin_user_pages(pages, pinned);
		kvfree(pages);
		return pinned < 0 ? pinned : -EFAULT;
	}

	mr->pages = pages;
	mr->flags |= IO_REGION_F_USER_PROVIDED;
	return 0;
}

static int io_region_pin_pages(struct io_mapped_region *mr,
			       struct io_uring_region_desc *reg)
{
	unsigned long size = (size_t) mr->nr_pages << PAGE_SHIFT;
	struct page **pages;
	int nr_pages;

	if (ppps_mm_is_compat(current->mm))
		return io_region_pin_compat_page(mr, reg);

	pages = io_pin_pages(reg->user_addr, size, &nr_pages);
	if (IS_ERR(pages))
		return PTR_ERR(pages);
	if (WARN_ON_ONCE(nr_pages != mr->nr_pages))
		return -EFAULT;

	mr->pages = pages;
	mr->flags |= IO_REGION_F_USER_PROVIDED;
	return 0;
}

static int io_region_allocate_pages(struct io_ring_ctx *ctx,
				    struct io_mapped_region *mr,
				    struct io_uring_region_desc *reg,
				    unsigned long mmap_offset)
{
	gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN;
	size_t size = (size_t) mr->nr_pages << PAGE_SHIFT;
	unsigned long nr_allocated;
	struct page **pages;
	void *p;

	pages = kvmalloc_array(mr->nr_pages, sizeof(*pages), gfp);
	if (!pages)
		return -ENOMEM;

	p = io_mem_alloc_compound(pages, mr->nr_pages, size, gfp);
	if (!IS_ERR(p)) {
		mr->flags |= IO_REGION_F_SINGLE_REF;
		goto done;
	}

	nr_allocated = alloc_pages_bulk_node(gfp, NUMA_NO_NODE,
					     mr->nr_pages, pages);
	if (nr_allocated != mr->nr_pages) {
		if (nr_allocated)
			release_pages(pages, nr_allocated);
		kvfree(pages);
		return -ENOMEM;
	}
done:
	reg->mmap_offset = mmap_offset;
	mr->pages = pages;
	return 0;
}

/* Track the userspace-visible size separately from native backing pages. */
int io_create_region(struct io_ring_ctx *ctx, struct io_mapped_region *mr,
		     struct io_uring_region_desc *reg,
		     unsigned long mmap_offset)
{
	u64 end, aligned_end, nr_pages;
	int ret;

	if (WARN_ON_ONCE(mr->pages || mr->ptr || mr->size || mr->nr_pages))
		return -EFAULT;
	if (memchr_inv(&reg->__resv, 0, sizeof(reg->__resv)))
		return -EINVAL;
	if (reg->flags & ~IORING_MEM_REGION_TYPE_USER)
		return -EINVAL;
	/* user_addr should be set IFF it's a user memory backed region */
	if ((reg->flags & IORING_MEM_REGION_TYPE_USER) != !!reg->user_addr)
		return -EFAULT;
	if (!reg->size || reg->mmap_offset || reg->id)
		return -EINVAL;
	if (reg->size > SIZE_MAX || reg->user_addr > ULONG_MAX)
		return -E2BIG;
	if (!MM_PAGE_ALIGNED(current->mm, reg->user_addr) ||
	    !MM_PAGE_ALIGNED(current->mm, reg->size))
		return -EINVAL;
	if (check_add_overflow(reg->user_addr, reg->size, &end))
		return -EOVERFLOW;
	if (check_add_overflow(end, PAGE_SIZE - 1, &aligned_end))
		return -EOVERFLOW;
	aligned_end &= PAGE_MASK;
	nr_pages = (aligned_end - (reg->user_addr & PAGE_MASK)) >>
		   PAGE_SHIFT;
	if (!nr_pages || nr_pages > INT_MAX)
		return -E2BIG;

	if (ctx->user) {
		ret = __io_account_mem(ctx->user, nr_pages);
		if (ret)
			return ret;
	}
	mr->nr_pages = nr_pages;
	mr->size = reg->size;

	if (reg->flags & IORING_MEM_REGION_TYPE_USER)
		ret = io_region_pin_pages(mr, reg);
	else
		ret = io_region_allocate_pages(ctx, mr, reg, mmap_offset);
	if (ret)
		goto out_free;

	ret = io_region_init_ptr(mr);
	if (ret)
		goto out_free;
	return 0;
out_free:
	io_free_region(ctx, mr);
	return ret;
}

int io_create_region_mmap_safe(struct io_ring_ctx *ctx, struct io_mapped_region *mr,
				struct io_uring_region_desc *reg,
				unsigned long mmap_offset)
{
	struct io_mapped_region tmp_mr;
	int ret;

	memcpy(&tmp_mr, mr, sizeof(tmp_mr));
	ret = io_create_region(ctx, &tmp_mr, reg, mmap_offset);
	if (ret)
		return ret;

	/*
	 * Once published mmap can find it without holding only the ->mmap_lock
	 * and not ->uring_lock.
	 */
	guard(mutex)(&ctx->mmap_lock);
	memcpy(mr, &tmp_mr, sizeof(tmp_mr));
	return 0;
}

static size_t io_ring_mmap_size(struct io_ring_ctx *ctx, loff_t offset)
{
	struct io_rings_layout layout;

	if (io_uring_calc_rings_size(ctx->flags, ctx->sq_entries,
				    ctx->cq_entries, &layout))
		return 0;

	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		return layout.rings_size;
	case IORING_OFF_SQES:
		return layout.sq_size;
	}
	return 0;
}

static struct io_mapped_region *io_mmap_get_region(struct io_ring_ctx *ctx,
						   loff_t offset,
						   size_t *mmap_size)
{
	struct io_mapped_region *region = NULL;
	unsigned int id;

	if (mmap_size)
		*mmap_size = 0;
	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		region = &ctx->ring_region;
		break;
	case IORING_OFF_SQES:
		region = &ctx->sq_region;
		break;
	case IORING_OFF_PBUF_RING:
		id = (offset & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_PBUF_SHIFT;
		return io_pbuf_get_region(ctx, id, mmap_size);
	case IORING_MAP_OFF_PARAM_REGION:
		region = &ctx->param_region;
		if (mmap_size)
			*mmap_size = io_region_size(region);
		break;
	case IORING_MAP_OFF_ZCRX_REGION:
		id = (offset & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_ZCRX_SHIFT;
		region = io_zcrx_get_region(ctx, id);
		if (region && mmap_size)
			*mmap_size = io_region_size(region);
		break;
	}
	if (region && mmap_size && !*mmap_size)
		*mmap_size = io_ring_mmap_size(ctx, offset);
	return region;
}

static void *io_region_validate_mmap(struct io_ring_ctx *ctx,
				     struct io_mapped_region *mr)
{
	lockdep_assert_held(&ctx->mmap_lock);

	if (!io_region_is_set(mr))
		return ERR_PTR(-EINVAL);
	if (mr->flags & IO_REGION_F_USER_PROVIDED)
		return ERR_PTR(-EINVAL);

	return io_region_get_ptr(mr);
}

static void *io_uring_validate_mmap_request(struct file *file, loff_t offset)
{
	struct io_ring_ctx *ctx = file->private_data;
	struct io_mapped_region *region;

	region = io_mmap_get_region(ctx, offset, NULL);
	if (!region)
		return ERR_PTR(-EINVAL);
	return io_region_validate_mmap(ctx, region);
}

#ifdef CONFIG_MMU

static int io_region_mmap(struct io_ring_ctx *ctx,
			  struct io_mapped_region *mr,
			  struct vm_area_struct *vma,
			  size_t mmap_size,
			  unsigned max_pages)
{
	size_t allowed_size = MM_PAGE_ALIGN(vma->vm_mm, mmap_size);
	unsigned long nr_pages = min(mr->nr_pages, max_pages);

	if (allowed_size < mmap_size || vma->vm_end - vma->vm_start > allowed_size)
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND);
	return vm_insert_pages(vma, vma->vm_start, mr->pages, &nr_pages);
}

__cold int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ring_ctx *ctx = file->private_data;
	size_t sz = vma->vm_end - vma->vm_start;
	loff_t offset = vma_file_offset(vma);
	unsigned int page_limit = UINT_MAX;
	struct io_mapped_region *region;
	size_t mmap_size;
	void *ptr;

	guard(mutex)(&ctx->mmap_lock);

	ptr = io_uring_validate_mmap_request(file, offset);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		page_limit = (sz + PAGE_SIZE - 1) >> PAGE_SHIFT;
		break;
	}

	region = io_mmap_get_region(ctx, offset, &mmap_size);
	if (!region || !mmap_size)
		return -EINVAL;
	return io_region_mmap(ctx, region, vma, mmap_size, page_limit);
}

unsigned long io_uring_get_unmapped_area(struct file *filp, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	struct io_ring_ctx *ctx = filp->private_data;
	loff_t offset = (loff_t)pgoff << MM_PAGE_SHIFT(current->mm);
	void *ptr;

	/*
	 * Do not allow to map to user-provided address to avoid breaking the
	 * aliasing rules. Userspace is not able to guess the offset address of
	 * kernel kmalloc()ed memory area.
	 */
	if (addr)
		return -EINVAL;

	guard(mutex)(&ctx->mmap_lock);

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
	struct io_ring_ctx *ctx = file->private_data;
	struct io_mapped_region *region;
	unsigned long i;

	if (!is_nommu_shared_mapping(vma->vm_flags))
		return -EINVAL;

	guard(mutex)(&ctx->mmap_lock);
	region = io_mmap_get_region(ctx,
				    (loff_t)vma->vm_pgoff << MM_PAGE_SHIFT(current->mm));
	if (!region || !io_region_is_set(region))
		return -EINVAL;

	if ((vma->vm_end - vma->vm_start) !=
	    (unsigned long) region->nr_pages << PAGE_SHIFT)
		return -EINVAL;

	/*
	 * Pin the pages so io_free_region()'s release_pages() does not
	 * drop the last reference while this VMA exists. delete_vma()
	 * in mm/nommu.c calls vma_close() which runs ->close above.
	 */
	for (i = 0; i < region->nr_pages; i++)
		get_page(region->pages[i]);

	vma->vm_ops = &io_uring_nommu_vm_ops;
	return 0;
}

unsigned int io_uring_nommu_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_DIRECT | NOMMU_MAP_READ | NOMMU_MAP_WRITE;
}

unsigned long io_uring_get_unmapped_area(struct file *file, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	struct io_ring_ctx *ctx = file->private_data;
	void *ptr;

	guard(mutex)(&ctx->mmap_lock);

	ptr = io_uring_validate_mmap_request(file,
					     (loff_t)pgoff << MM_PAGE_SHIFT(current->mm));
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	return (unsigned long) ptr;
}

#endif /* !CONFIG_MMU */
