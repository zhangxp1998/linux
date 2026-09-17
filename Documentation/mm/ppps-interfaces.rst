.. SPDX-License-Identifier: GPL-2.0

======================
PPPS interface units
======================

Per-process page sizes separate user mapping geometry from native backing
pages. Pass byte addresses, offsets and lengths across subsystem interfaces.
Convert to a page index only at the operation which requires that index.
Always use the target mm, not implicitly current->mm, for remote operations.

Personality selector
====================

ADDR_4KB_COMPAT_PAGE_SIZE uses bit 30 (0x40000000), the highest bit below the
sign bit of libc's int personality() return value. Keeping the selector away
from the currently allocated low flags leaves room for future additions;
it does not reserve this bit against future upstream allocation.

Setting or clearing this flag selects the page size at the next exec. It
does not change the current mm or the page size inherited by fork. Preserve
unrelated personality flags when changing the selector. Launchers and tests
built for the previous 0x10000000 value must be rebuilt; the previous value
is not retained as a PPPS alias.

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

Module interfaces and backing alignment
=======================================

An EXPORT_SYMBOL declaration is not a vendor KMI guarantee. In this branch,
vm_insert_page_native() is exported for in-tree consumers such as Rust Binder,
but is not in the Pixel GKI symbol list. In-tree module dependencies can keep
it through symbol trimming without making it available as a supported vendor
interface. Vendor modules must use helpers included in their target kernel's
KMI list; adding a new dependency requires a separate KMI update.

vm_insert_page_native() inserts a whole native page, whereas
vm_insert_page_slice() identifies one physical slice explicitly. Neither is
a substitute for the other without reviewing the mapping's byte geometry.

udmabuf can export 4K-aligned subranges for compat processes. Importers with
a native-page-sized backing contract must validate that contract before
initializing their own objects. In particular, a custom GEM importer which
bypasses drm_gem_dma_prime_import_sg_table() must check the dma-buf size
before drm_gem_private_object_init(). The common helper's validation does
not cover private importer callbacks. Rejecting an unsupported import does
not require disabling 4K udmabuf exports globally.

RSS readers
===========

Raw FILE/SHMEM/SWAP counters count process-page mappings; ANON uses
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

File PSS fast path
==================

A page_ext counter records the number of compat file PTEs referencing each
native page. It is not a process count, and is never divided by the number
of slices to approximate PSS. A native reader may use the existing native
mapcount when this counter is zero. With exactly one compat PTE and N native
mappings, the compat slice has N+1 mappings and the other three have N.
A native PTE's PSS is therefore 12KiB/N + 4KiB/(N+1), while the compat PTE's
PSS is 4KiB/(N+1). Neither requires locating the single shared slice. The
native entry is not exclusive; the compat entry is exclusive only if N=0.
The implementation retains per-slice fixed-point rounding and private/shared
byte accounting, rather than charging all 16KiB at a single divisor.
Larger compat counts retain the per-slice reverse-map walk and the existing
PSS and pagemap exclusivity semantics.
One process mapping all four slices and four processes mapping one slice
can have equal counters but different sharing distributions.

Additions (including fork's file-rmap duplication) increment the counter
before the ordinary rmap update. Removal decrements it after the rmap update.
Moving a PTE in the same mm does not change the count; migration removes the
old page's mappings and adds the new page's mappings. Folio splitting keeps
the counters associated with the same native pages. Missing metadata and
saturated counters retain the slow path. Like native mapcounts and the
existing reverse-map walk, these statistics are not atomic snapshots of
all processes; the counter is not an ownership or memory-safety primitive.

The counter has a 32-bit payload in an 8-byte-aligned page_ext slot on arm64
(about 4 MiB for 8 GiB of 16K pages, in addition to other page_ext clients).
No struct page layout, public rmap function signature or proc unit changes.
The selftest smaps_native_fallback_ppps checks zero/single/multiple compat
PTEs, mixed sharing, partial unmap, mremap and fork/exit transitions using
normal userspace interfaces.

Pagemap entries
===============

/proc/PID/pagemap is indexed in the target process's page units. The PFN
field remains a native PAGE_SHIFT PFN: four compat entries may identify the
same 16K backing page. It does not encode the physical slice offset, so a
compat pagemap entry alone is not a complete byte physical address. Kernel
consumers needing the exact byte range must use the page_span returned by
the range GUP interfaces rather than infer the slice from a PFN or a VMA.

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

External shmem population
=========================

UFFD missing registration retains per-slice population while access remains
inside that mechanism. Refaulting an already filled slice does not populate
its siblings. Ordinary file reads, writes, splice, allocation and faults
through other VMAs instead adopt native 16K cache population: a partially
filled folio loses its slice tracking entry, and untouched bytes are zero.
This does not install sibling PTEs or change 4K VMA/protection boundaries.

The same rule applies when memfd_pin_folios() or uprobes consume a user shmem
folio. Hold the folio lock when forgetting its entry. An unlocked pin or
reference does not guarantee page-cache membership: acquire the lock, then
re-read and validate folio_mapping() before using its inode and index.
Do not acquire the inode UFFD mutex while holding the folio lock. Internal
truncate/hole lookups and UFFD's SGP_NOALLOC path do not adopt this policy.

Driver-RAM accounting
=====================

A driver mapping may supply normal pages without a page-cache mapping.
The slice reverse walk cannot recover their sharing distribution. In that
case use folio_precise_page_mapcount() for the selected native page, not the
whole large folio. This conservative fallback avoids reporting shared RAM
as exclusive, but can underestimate compat PSS when several slice PTEs
belong to one process; it is not precise per-process or per-slice accounting.
