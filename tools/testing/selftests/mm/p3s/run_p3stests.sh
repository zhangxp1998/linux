#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run the complete P3S selftest matrix from this directory or an install tree.

set -u
cd "$(dirname "$0")" || exit 1
export TMPDIR="${TMPDIR:-/tmp}"

list_only=false
case_index=
case "${1:-}" in
--list|-l) list_only=true; shift ;;
--case)
	case_index=${2:-}
	case "$case_index" in
		''|*[!0-9]*|0) echo "invalid case index: $case_index" >&2; exit 2 ;;
	esac
	shift 2 ;;
--help|-h)
	echo "usage: $0 [--list|--case N]"
	echo "Build the P3S targets in their owning selftest directories (see README.p3s), then run here or in the installed mm/p3s directory."
	exit 0 ;;
"") ;;
*) echo "usage: $0 [--list|--case N]" >&2; exit 2 ;;
esac
[ "$#" -eq 0 ] || { echo "unexpected arguments: $*" >&2; exit 2; }

count_total=0
count_pass=0
count_fail=0
count_skip=0
case_ordinal=0
log_file=
mounted_debugfs=false

cleanup()
{
	status=$?
	trap - EXIT
	[ -z "$log_file" ] || rm -f "$log_file"
	if $mounted_debugfs && ! umount /sys/kernel/debug; then
		echo "could not unmount test-owned debugfs" >&2
		status=1
	fi
	exit "$status"
}

if ! $list_only; then
	log_file=$(mktemp "$TMPDIR/p3s-run.XXXXXX") || exit 1
	trap cleanup EXIT
fi

run_test()
{
	name="$*"
	case_ordinal=$((case_ordinal + 1))
	if [ -n "$case_index" ] && [ "$case_ordinal" -ne "$case_index" ]; then
		return
	fi
	if $list_only; then
		printf '%s\n' "$name"
		return
	fi
	count_total=$((count_total + 1))
	printf '# RUN %s\n' "$name"
	# Android has /system/bin/sh but no /bin/sh.  Keep the scripts
	# portable for kselftest on hosts by invoking them through sh.
	case $1 in
	*.sh) sh "$@" >"$log_file" 2>&1 ;;
	*) "$@" >"$log_file" 2>&1 ;;
	esac
	status=$?
	# kselftest_harness exits 0 even when every TEST reported SKIP.
	# Keep the matrix's pass/skip counts faithful to its final TAP summary.
	if [ "$status" -eq 0 ]; then
		totals=$(grep '^# Totals:' "$log_file" | tail -n 1)
		if printf '%s\n' "$totals" |
		   grep -Eq '^# Totals: pass:0 fail:0 xfail:0 xpass:0 skip:[1-9][0-9]* error:0$'; then
			status=4
		fi
	fi
	sed 's/^/# /' "$log_file"
	case $status in
	0) count_pass=$((count_pass + 1))
	   printf 'ok %d - %s\n' "$count_total" "$name" ;;
	4) count_skip=$((count_skip + 1))
	   printf 'ok %d - %s # SKIP\n' "$count_total" "$name" ;;
	*) count_fail=$((count_fail + 1))
	   printf 'not ok %d - %s # exit=%d\n' "$count_total" "$name" "$status" ;;
	esac
}

run_p3s_module_test()
{
	module=$1
	shift
	case "$module" in
	dma_mmap_*|iommu_dma_mmap) module_dir=../../dma/p3s/p3s_modules ;;
	vb2_*) module_dir=../../media_tests/p3s/p3s_modules ;;
	*) module_dir=p3s_modules ;;
	esac
	run_test ./p3s_run_module.sh "${module_dir}/${module}_p3s_module.ko" \
		"/dev/${module}_p3s" -- "$@"
}

run_p3s_module2_test()
{
	module1=$1 module2=$2
	shift 2
	run_p3s_module_test "$module1" sh ./p3s_run_module.sh \
		"p3s_modules/${module2}_p3s_module.ko" \
		"/dev/${module2}_p3s" -- "$@"
}

if ! $list_only; then
	printf 'TAP version 13\n'
	if [ "$(uname -m)" != aarch64 ]; then
		printf '1..0 # SKIP P3S requires an AArch64 kernel\n'
		exit 4
	fi
	if [ -d /sys/kernel/debug ] &&
	   ! grep -q ' /sys/kernel/debug debugfs ' /proc/mounts 2>/dev/null; then
		if mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; then
			mounted_debugfs=true
		fi
	fi
fi

run_test ./mglru_folded_p3s
run_test ./page_size_contract_p3s
run_test ./page_size_contract_p3s --native
run_test ../../exec/p3s/elf_4k_align_p3s
run_test ../../coredump/p3s/elf_core_p3s
run_test ../../proc/p3s/taskstats_p3s
run_test ../../proc/p3s/rusage_p3s
run_test ../../proc/p3s/oom_anon_score_p3s
run_test ../../io_uring/p3s/io_uring_mmap_p3s
run_test ./stack_guard_gap_p3s
run_test ./mglru_p3s
run_test ./mmu_notifier_p3s.sh
run_test ./cow_notifier_p3s.sh
run_test ./anon_pageout_p3s
run_test ./madvise_free_p3s
run_test ./madvise_free_holes_p3s
run_test ./madvise_free_holes_p3s --native
run_test ./madvise_guard_p3s
run_test ./thp_mmap_offset_p3s
run_test ./mthp_native_anon_p3s
run_test ./devzero_mthp_p3s
run_test ./shmem_mmap_offset_p3s
run_test ../../memfd/p3s/memfd_eof_p3s secretmem
run_test ../../memfd/p3s/memfd_eof_p3s shmem
run_test ./mincore_p3s
run_test ./mincore_p3s --native
run_p3s_module_test gup_retry ./mmap_action_cleanup_p3s
run_test ../../lsm/p3s/selinux_status_mmap_p3s.sh
run_p3s_module2_test vm_iomap_memory vm_iomap_unaligned ./vm_iomap_memory_p3s
run_p3s_module2_test vm_iomap_memory vm_iomap_unaligned ./vm_iomap_memory_p3s --native
run_test ../../dmabuf-heaps/p3s/dma_heap_p3s --native
run_test ../../dmabuf-heaps/p3s/dma_heap_p3s
run_p3s_module_test remap_pfn_cow ./remap_pfn_cow_p3s --native
run_p3s_module_test remap_pfn_cow ./remap_pfn_cow_p3s
run_p3s_module_test pfn_insert ./pfn_insert_p3s --native
run_p3s_module_test pfn_insert ./pfn_insert_p3s
run_p3s_module_test dma_mmap_attrs ../../dma/p3s/dma_mmap_p3s attrs
run_p3s_module_test dma_mmap_pages ../../dma/p3s/dma_mmap_p3s pages
run_p3s_module_test dma_mmap_attrs ../../dma/p3s/dma_mmap_full_p3s attrs
run_p3s_module_test dma_mmap_pages ../../dma/p3s/dma_mmap_full_p3s pages
run_p3s_module_test dma_mmap_bounds ../../dma/p3s/dma_mmap_bounds_p3s
run_p3s_module_test dma_mmap_bounds ../../dma/p3s/dma_mmap_bounds_p3s --native
run_test ../../dma/p3s/iommu_dma_mmap_p3s.sh
run_p3s_module_test vm_map_pages ./vm_map_pages_p3s
run_p3s_module_test vb2_mmap sh -c '../../media_tests/p3s/vb2_mmap_p3s --native && exec ../../media_tests/p3s/vb2_mmap_p3s'
run_test ../../drivers/gpu/p3s/drm_gem_slice_p3s --native
run_test ../../drivers/gpu/p3s/drm_gem_slice_p3s
run_test ./mmap_hint_p3s
run_test ../../exec/p3s/brk_aslr_p3s
run_test ../../exec/p3s/mmap_aslr_p3s
run_test ../../exec/p3s/exec_aslr_p3s
run_test ./shmem_thp_mmap_p3s
run_test ./smaps_page_size_p3s
run_test ./fork_page_size_p3s
run_test ./mmap_page_zero_p3s
run_test ../../exec/p3s/exec_stack_random_p3s
run_test ./vma_range_slice_p3s
run_test ./vma_merge_next_pgoff_p3s
run_test ./vma_merge_file_slice_p3s
run_test ./vma_merge_devzero_p3s --native
run_test ./vma_merge_devzero_p3s
run_test ../../exec/p3s/stack_aslr_p3s
run_test ../../perf_events/p3s/perf_mmap_offset_p3s
run_test ./statm_cross_geometry_p3s
run_test ../../pci/p3s/pci_mmap_p3s
run_test ./anon_vma_name_p3s
run_test ./print_vma_addr_p3s
run_p3s_module_test pagewalk ./pagewalk_p3s
run_p3s_module_test folio_within_range ./folio_within_range_p3s
run_test ./remap_file_pages_p3s
run_test ./shmem_swap_usage_p3s
run_test ./swap_header_p3s
run_test ./swap_header_badpages_p3s
run_test ./swapoff_shared_slice_p3s
run_test ./swap_pss_p3s
run_test ./anon_rmap_split_window_p3s
run_test ./fork_dontfork_slice_p3s
run_test ../../ring-buffer/p3s/trace_ring_buffer_mmap_p3s --native
run_test ../../ring-buffer/p3s/trace_ring_buffer_mmap_p3s
run_test ./truncate_p3s
run_p3s_module_test gup_retry ./gup_retry_p3s
run_p3s_module_test iov_iter ./iov_iter_p3s
run_p3s_module_test iov_range_retry ./iov_range_retry_p3s
run_p3s_module_test iov_range_retry ./iov_range_retry_p3s --native
run_p3s_module_test iov_iter ./page_range_p3s
run_p3s_module_test iov_iter ./page_range_p3s --native
run_p3s_module_test fault_in ./fault_in_p3s
run_test ../../splice/p3s/vmsplice_slice_p3s
run_test ../../futex/p3s/futex_shared_p3s
run_test ../../futex/p3s/futex_uffd_p3s
run_test ../../futex/p3s/futex_uffd_p3s --native
run_test ../../net/p3s/xdp_umem_p3s --native
run_test ../../net/p3s/xdp_umem_p3s
run_test ../../net/p3s/tcp_zerocopy_align_p3s
run_test ./userfaultfd_mixed_p3s
run_test ./userfaultfd_remap_p3s
run_test ./userfaultfd_shmem_p3s
run_test ./uffd_move_policy_p3s
run_test ./mbind_p3s
run_test ./mlock_onfault_p3s
run_test ./zero_sibling_mlock_p3s
run_test ./mlock_onfault_p3s --native
run_test ./mlock_large_file_p3s
run_test ./mlock_file_slices_p3s
run_test ./mlock_file_slices_p3s --native
run_test ./mlock_reclaim_p3s
run_test ./mremap_rlimit_p3s
run_test ./mremap_commit_p3s
run_test ./mremap_hint_p3s
run_p3s_module_test mremap_pgoff ./mremap_pgoff_p3s
run_test ./msync_offset_p3s
run_test ../../bpf/p3s/bpf_map_mmap_p3s
run_test ../../bpf/p3s/bpf_map_mmap_p3s --native
run_test ../../bpf/p3s/bpf_remote_access_p3s --native
run_test ./fork_filemap_p3s
run_test ./munmap_p3s
run_test ../../bpf/p3s/bpf_stack_build_id_p3s
run_test ./madvise_remove_p3s
run_test ./partial_anon_reclaim_p3s
run_test ./madvise_willneed_p3s "$TMPDIR/madvise-willneed-p3s.bin"
run_test ./process_madvise_p3s
run_test ./process_vm_access_p3s
run_test ./remote_write_dontneed_p3s
run_test ./mixed_context_switch_p3s
run_test ./mixed_mm_access_p3s
run_test ./pagemap_ioctl_p3s
run_test ./pagemap_scan_p3s
run_test ../../bpf/p3s/btf_vmlinux_mmap_p3s
run_test ../../alsa/p3s/alsa_pcm_mmap_p3s
run_test ../../pci/p3s/access_phys_p3s
run_test ./devzero_slice_p3s
run_test ../../ftrace/p3s/uprobe_p3s
run_test ../../perf_events/p3s/perf_page_size_p3s
run_test ./pagemap_exclusive_p3s
run_test ./smaps_pss_p3s
run_test ./uffd_move_swap_window_p3s
run_test ./uffd_move_swap_window_p3s --native
run_test ../../android/p3s/ashmem_p3s
run_test ./dontneed_refault_p3s
run_test ../../exec/p3s/exec_env_p3s
run_test ../../arm64/mte/p3s/mte_anon_p3s
run_test ../../exec/p3s/exec_user_args_p3s
run_test ./file_cow_p3s
run_p3s_module_test iov_iter ./file_cow_refs_p3s
run_test ./filemap_eof_p3s "${TMPDIR:-/tmp}"
run_test ./hole_anon_p3s
run_test ../../kcov/p3s/kcov_p3s
run_test ./madv_cold_large_file_p3s "${TMPDIR:-/tmp}/madv-cold-p3s.bin"
run_p3s_module_test anon_exclusive_probe ./memory_failure_anon_p3s
run_test ./mremap_file_slice_p3s
run_test ./mremap_slice_cow_p3s
run_test ./mremap_split_p3s
run_test ../../proc/p3s/oom_score_p3s
run_test ./packed_anon_p3s
run_test ./packed_swap_p3s
run_test ./proc_pagewalk_p3s
run_test ./process_mrelease_p3s
run_test ./rmap_file_referenced_p3s "${TMPDIR:-/tmp}/rmap-file-p3s.bin"
run_test ./smaps_native_fallback_p3s
run_test ./swap_badpages_loop_p3s
run_test ./swap_slice_mremap_p3s
run_test ./swap_vma_readahead_p3s
run_test ../../net/p3s/tcp_zerocopy_reject_p3s
run_test ./truncate_remote_p3s
run_test ./truncate_tail_p3s "${TMPDIR:-/tmp}"
run_test ./uffd_copy_tuple_p3s
run_test ./uffd_sigbus_anon_p3s
run_test ./userfaultfd_poison_remote_p3s
run_test ./userfaultfd_remote_p3s
run_test ./userfaultfd_unregister_remote_p3s
run_test ./userfaultfd_wp_remote_p3s
run_p3s_module_test vb2_userptr_reject ../../media_tests/p3s/vb2_userptr_reject_p3s
run_test ./vmalloc_align_p3s.sh

if ! $list_only; then
	if [ -n "$case_index" ] && [ "$count_total" -ne 1 ]; then
		echo "case $case_index is out of range" >&2
		exit 2
	fi
	printf '1..%d\n' "$count_total"
	printf '# P3S: %d passed, %d skipped, %d failed\n' \
		"$count_pass" "$count_skip" "$count_fail"
	[ "$count_fail" -eq 0 ] || exit 1
	[ "$count_pass" -ne 0 ] || exit 4
fi
