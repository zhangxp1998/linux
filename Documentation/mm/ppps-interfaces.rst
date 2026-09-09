.. SPDX-License-Identifier: GPL-2.0

======================
PPPS interface units
======================

Per-process page sizes separate user mapping geometry from native backing
pages. Pass byte addresses, offsets and lengths across subsystem interfaces.
Convert to a page index only at the operation which requires that index.
Always use the target mm, not implicitly current->mm, for remote operations.

VMA offsets
===========

Use vma_file_offset() and vma_addr_file_offset() to read file/object offsets
in bytes. Use vma_set_file_offset() to assign a process-page-aligned byte
offset, including before a driver's mmap callback installs vm_ops. Failure
leaves the stored offset unchanged.

Anonymous indices and driver selectors are different domains. In particular,
an encoded pgoff can occupy more bits than its hypothetical byte offset.
vma_get_offset()/vma_set_offset() preserve the complete representation without
converting it to bytes. Use them for save/restore and VMA duplication.

VMA split/move code uses vma_offset_at() with a process-page-aligned address;
merge code can use vma_offset_advance() with an aligned byte displacement.
The latter explicitly distinguishes sliced file offsets from anonymous page
indices. Hold the locks required to modify the VMA, or operate on an
unpublished VMA. These helpers do not acquire locks themselves.

Pinned byte ranges
==================

pin_user_pages_range() and get_user_pages_range() accept a target mm, user
byte address and byte length. mm_user_range_pages() computes the necessary
array capacity without overflowing an intermediate rounded byte length.

Each returned array entry consists of a native page pointer and a page_span:
offset and length are both bytes within that page. The helper clips the first
and last entries. Callers must not add the original address displacement a
second time. Drivers can feed the spans directly into bvec/SG construction.

The return value is an entry count, not a byte count or a unique-page count.
A positive short result owns only that prefix of both arrays; summing its
lengths gives the completed byte count. Errors and empty ranges own nothing.
Every returned pin/reference must be released once, even when page pointers
repeat. A caller that coalesces adjacent spans must separately account for
the references it releases during coalescing.

Release pin_user_pages_range() results with unpin_user_page(s), and
get_user_pages_range() results with put_page()/release_pages(). Do not mix the
two ownership protocols. The helpers acquire mmap_read_lock as needed;
callers must not already hold that lock. Native local-mm requests retain
fast GUP, while compat/remote requests keep VMA geometry stable through span
construction. GUP's usual restrictions on mapping types and flags remain.

The older pin_user_pages_with_offsets() and get_vaddr_frames() count-based
interfaces remain compatibility wrappers. New call sites should use the
byte-range interfaces rather than reconstructing byte spans themselves.

RSS readers
===========

Raw FILE/SHMEM counters count process-page mappings; ANON/SWAP counters use
native-page units. This storage choice belongs to accounting code, not to
proc, trace or OOM consumers.

get_mm_counter_bytes() retains an approximate read; get_mm_counter_sum_bytes()
retains the full-sum read. mm_counter_to_bytes() converts an already-read raw
counter. get_mm_rss_bytes() combines the resident counters, and
mm_process_pages_to_bytes() converts VM sizes and their high-water marks.
Convert bytes to the output ABI's units only at the output boundary.

get_mm_oom_pages() centralizes the native-page footprint used by OOM scoring:
round the combined resident/swap bytes once, then add the page-table count.
Do not round the file and shmem components independently. These reads are
not atomic snapshots and do not change the synchronization or rounding
guarantees of the underlying counters. Storage units and proc/trace ABIs
are unchanged.

Native-only device APIs
======================

The ublk server/control API, VFIO and KVM host API are outside the PPPS
compat interface. A process with a PPPS 4K mm receives -EOPNOTSUPP when it
opens their device nodes. Their command/ioctl and mmap entry points also
reject compat callers using inherited or transferred native-opened FDs;
ublk channel read/write operations are checked as well. Release and request
cancellation still run normally. Normal VFS and security permission checks
may reject access before a driver is reached.

Use ppps_mm_is_compat() on the current or mapping mm for these checks, not
the personality bit (which selects the next exec) or the 32-bit syscall ABI.
Native processes, including native 4K kernels without PPPS, retain their
existing interfaces. KVM guest page sizes are unaffected. Access to ordinary
files on filesystems backed by ublk is not restricted by this device policy;
the kernel block I/O path is not gated on the submitting worker's mm.

The policy avoids compat device mappings; it is not a new security boundary
for already-delegated generic block FDs, metadata operations or shared memory.
Android device permissions and SELinux remain responsible for app isolation.
