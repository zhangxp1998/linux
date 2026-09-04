// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * linux/mm/process_vm_access.c
 *
 * Copyright (C) 2010-2011 Christopher Yeoh <cyeoh@au1.ibm.com>, IBM Corp.
 */

#include <linux/compat.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

/**
 * process_vm_rw_pages - read/write pages from task specified
 * @pages: array of pointers to pages we want to copy
 * @offset: offset in page to start copying from/to
 * @len: number of bytes to copy
 * @iter: where to copy to/from locally
 * @page_size: page size of the target process
 * @slice_idx: first subpage slice within the host page
 * @advance_slice: whether each target page advances the subpage slice
 * @vm_write: 0 means copy from, 1 means copy to
 * Returns 0 on success, error code otherwise
 */
static int process_vm_rw_pages(struct page **pages,
			       unsigned offset,
			       size_t len,
			       struct iov_iter *iter,
			       unsigned int page_size,
			       unsigned int slice_idx,
			       bool advance_slice,
			       int vm_write)
{
	/* Do the copy for each page */
	while (len && iov_iter_count(iter)) {
		struct page *page = *pages++;
		unsigned int page_offset = slice_idx * page_size + offset;
		size_t copy = page_size - offset;
		size_t copied;

		if (copy > len)
			copy = len;

		if (vm_write)
			copied = copy_page_from_iter(page, page_offset, copy, iter);
		else
			copied = copy_page_to_iter(page, page_offset, copy, iter);

		len -= copied;
		if (copied < copy && iov_iter_count(iter))
			return -EFAULT;
		offset = 0;
		if (advance_slice)
			slice_idx = (slice_idx + 1) & PPPS_SLICE_MASK;
	}
	return 0;
}

/* Maximum number of pages kmalloc'd to hold struct page's during copy */
#define PVM_MAX_KMALLOC_PAGES 2

/* Maximum number of pages that can be stored at a time */
#define PVM_MAX_USER_PAGES (PVM_MAX_KMALLOC_PAGES * PAGE_SIZE / sizeof(struct page *))

/**
 * process_vm_rw_single_vec - read/write pages from task specified
 * @addr: start memory address of target process
 * @len: size of area to copy to/from
 * @iter: where to copy to/from locally
 * @process_pages: struct pages area that can store at least
 *  nr_pages_to_copy struct page pointers
 * @mm: mm for task
 * @task: task to read/write from
 * @vm_write: 0 means copy from, 1 means copy to
 * Returns 0 on success or on failure error code
 */
static int process_vm_rw_single_vec(unsigned long addr,
				    unsigned long len,
				    struct iov_iter *iter,
				    struct page **process_pages,
				    struct mm_struct *mm,
				    struct task_struct *task,
				    int vm_write)
{
	unsigned long page_size = MM_PAGE_SIZE(mm);
	unsigned int page_shift = MM_PAGE_SHIFT(mm);
	unsigned long pa;
	unsigned long start_offset;
	unsigned long nr_pages;
	ssize_t rc = 0;
	unsigned int flags = 0;

	/* Work out address and page range required */
	if (len == 0)
		return 0;
	mmap_read_lock(mm);
	addr = untagged_addr_remote(mm, addr);
	mmap_read_unlock(mm);
	pa = addr & MM_PAGE_MASK(mm);
	start_offset = addr - pa;
	nr_pages = (addr + len - 1) / page_size - addr / page_size + 1;

	if (vm_write)
		flags |= FOLL_WRITE;

	while (!rc && nr_pages && iov_iter_count(iter)) {
		int pinned_pages = min_t(unsigned long, nr_pages, PVM_MAX_USER_PAGES);
		struct vm_area_struct *vma;
		unsigned long vma_pages;
		unsigned int slice_idx;
		bool advance_slice = ppps_mm_is_compat(mm);
		int locked = 1;
		size_t bytes;

		/*
		 * Get the pages we're interested in.  We must
		 * access remotely because task/mm might not
		 * current/current->mm
		 */
		if (mmap_read_lock_killable(mm))
			return -EINTR;
		vma = vma_lookup(mm, pa);
		if (!vma) {
			mmap_read_unlock(mm);
			return -EFAULT;
		}
		slice_idx = vma_address_to_slice(vma, pa);
		vma_pages = (vma->vm_end - pa) >> page_shift;
		pinned_pages = min_t(unsigned long, pinned_pages, vma_pages);
		pinned_pages = pin_user_pages_remote(mm, pa, pinned_pages,
						     flags, process_pages,
						     &locked);
		if (locked)
			mmap_read_unlock(mm);
		if (pinned_pages <= 0)
			return -EFAULT;
		/*
		 * A fault may drop mmap_lock for UFFD or I/O.  Native pages need
		 * no VMA metadata, but compat offsets must describe these exact
		 * pins.  If geometry could have changed, discard the pins and
		 * retry the now-faulted range under a fresh lock/VMA lookup.
		 */
		if (!locked && advance_slice) {
			unpin_user_pages(process_pages, pinned_pages);
			cond_resched();
			continue;
		}

		bytes = pinned_pages * page_size - start_offset;
		if (bytes > len)
			bytes = len;

		rc = process_vm_rw_pages(process_pages,
					 start_offset, bytes, iter, page_size,
					 slice_idx, advance_slice,
					 vm_write);
		len -= bytes;
		start_offset = 0;
		nr_pages -= pinned_pages;
		pa += pinned_pages * page_size;

		/* If vm_write is set, the pages need to be made dirty: */
		unpin_user_pages_dirty_lock(process_pages, pinned_pages,
					    vm_write);
	}

	return rc;
}

/* Maximum number of entries for process pages array
   which lives on stack */
#define PVM_MAX_PP_ARRAY_COUNT 16

/**
 * process_vm_rw_core - core of reading/writing pages from task specified
 * @pid: PID of process to read/write from/to
 * @iter: where to copy to/from locally
 * @rvec: iovec array specifying where to copy to/from in the other process
 * @riovcnt: size of rvec array
 * @flags: currently unused
 * @vm_write: 0 if reading from other process, 1 if writing to other process
 *
 * Returns the number of bytes read/written or error code. May
 *  return less bytes than expected if an error occurs during the copying
 *  process.
 */
static ssize_t process_vm_rw_core(pid_t pid, struct iov_iter *iter,
				  const struct iovec *rvec,
				  unsigned long riovcnt,
				  unsigned long flags, int vm_write)
{
	struct task_struct *task;
	struct page *pp_stack[PVM_MAX_PP_ARRAY_COUNT];
	struct page **process_pages = pp_stack;
	struct mm_struct *mm;
	unsigned long i;
	ssize_t rc = 0;
	unsigned long nr_pages = 0;
	unsigned long nr_pages_iov;
	ssize_t iov_len;
	size_t total_len = iov_iter_count(iter);
	unsigned long page_size;

	/* Avoid looking up the target task for an empty remote vector. */
	for (i = 0; i < riovcnt; i++) {
		iov_len = rvec[i].iov_len;
		if (iov_len > 0)
			break;
	}
	if (i == riovcnt)
		return 0;

	/* Get process information */
	task = find_get_task_by_vpid(pid);
	if (!task) {
		rc = -ESRCH;
		goto free_proc_pages;
	}

	mm = mm_access(task, PTRACE_MODE_ATTACH_REALCREDS);
	if (!mm || IS_ERR(mm)) {
		rc = IS_ERR(mm) ? PTR_ERR(mm) : -ESRCH;
		/*
		 * Explicitly map EACCES to EPERM as EPERM is a more
		 * appropriate error code for process_vw_readv/writev
		 */
		if (rc == -EACCES)
			rc = -EPERM;
		goto put_task_struct;
	}
	page_size = MM_PAGE_SIZE(mm);

	/* Work out how many pages of struct pages we're going to need
	 * when eventually calling get_user_pages
	 */
	for (i = 0; i < riovcnt; i++) {
		iov_len = rvec[i].iov_len;
		if (iov_len > 0) {
			nr_pages_iov = ((unsigned long)rvec[i].iov_base
					+ iov_len - 1)
				/ page_size - (unsigned long)rvec[i].iov_base
				/ page_size + 1;
			nr_pages = max(nr_pages, nr_pages_iov);
		}
	}

	if (nr_pages > PVM_MAX_PP_ARRAY_COUNT) {
		/* For reliability don't try to kmalloc more than
		   2 pages worth */
		process_pages = kmalloc(min_t(size_t, PVM_MAX_KMALLOC_PAGES * PAGE_SIZE,
					      sizeof(struct page *)*nr_pages),
					GFP_KERNEL);
		if (!process_pages) {
			rc = -ENOMEM;
			goto put_mm;
		}
	}

	for (i = 0; i < riovcnt && iov_iter_count(iter) && !rc; i++)
		rc = process_vm_rw_single_vec(
			(unsigned long)rvec[i].iov_base, rvec[i].iov_len,
			iter, process_pages, mm, task, vm_write);

	/* copied = space before - space after */
	total_len -= iov_iter_count(iter);

	/* If we have managed to copy any data at all then
	   we return the number of bytes copied. Otherwise
	   we return the error code */
	if (total_len)
		rc = total_len;

put_mm:
	mmput(mm);

put_task_struct:
	put_task_struct(task);

free_proc_pages:
	if (process_pages != pp_stack)
		kfree(process_pages);
	return rc;
}

/**
 * process_vm_rw - check iovecs before calling core routine
 * @pid: PID of process to read/write from/to
 * @lvec: iovec array specifying where to copy to/from locally
 * @liovcnt: size of lvec array
 * @rvec: iovec array specifying where to copy to/from in the other process
 * @riovcnt: size of rvec array
 * @flags: currently unused
 * @vm_write: 0 if reading from other process, 1 if writing to other process
 *
 * Returns the number of bytes read/written or error code. May
 *  return less bytes than expected if an error occurs during the copying
 *  process.
 */
static ssize_t process_vm_rw(pid_t pid,
			     const struct iovec __user *lvec,
			     unsigned long liovcnt,
			     const struct iovec __user *rvec,
			     unsigned long riovcnt,
			     unsigned long flags, int vm_write)
{
	struct iovec iovstack_l[UIO_FASTIOV];
	struct iovec iovstack_r[UIO_FASTIOV];
	struct iovec *iov_l = iovstack_l;
	struct iovec *iov_r;
	struct iov_iter iter;
	ssize_t rc;
	int dir = vm_write ? ITER_SOURCE : ITER_DEST;

	if (flags != 0)
		return -EINVAL;

	/* Check iovecs */
	rc = import_iovec(dir, lvec, liovcnt, UIO_FASTIOV, &iov_l, &iter);
	if (rc < 0)
		return rc;
	if (!iov_iter_count(&iter))
		goto free_iov_l;
	iov_r = iovec_from_user(rvec, riovcnt, UIO_FASTIOV, iovstack_r,
				in_compat_syscall());
	if (IS_ERR(iov_r)) {
		rc = PTR_ERR(iov_r);
		goto free_iov_l;
	}
	rc = process_vm_rw_core(pid, &iter, iov_r, riovcnt, flags, vm_write);
	if (iov_r != iovstack_r)
		kfree(iov_r);
free_iov_l:
	kfree(iov_l);
	return rc;
}

SYSCALL_DEFINE6(process_vm_readv, pid_t, pid, const struct iovec __user *, lvec,
		unsigned long, liovcnt, const struct iovec __user *, rvec,
		unsigned long, riovcnt,	unsigned long, flags)
{
	return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, 0);
}

SYSCALL_DEFINE6(process_vm_writev, pid_t, pid,
		const struct iovec __user *, lvec,
		unsigned long, liovcnt, const struct iovec __user *, rvec,
		unsigned long, riovcnt,	unsigned long, flags)
{
	return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, 1);
}
