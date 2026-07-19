// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Kelvin Zhang */

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 2);
#ifdef __TARGET_ARCH_arm64
	__ulong(map_extra, 0x100000000ULL);
#else
	__ulong(map_extra, 0x100000000000ULL);
#endif
} arena SEC(".maps");

char arena_blob[5000] SEC(".addr_space.1") = {
	[0] = 0x31,
	[4999] = 0x7a,
};

SEC("socket")
int arena_ppps_noop(void *ctx)
{
	return !!ctx;
}

char _license[] SEC("license") = "GPL";
