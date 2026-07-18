// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define OUTPUT_SIZE 32

char _license[] SEC("license") = "GPL";

struct bpf_copy_state {
	u32 target_pid;
	u32 seen;
	u64 anon_ptr;
	u64 file_ptr;
	s32 anon_ret;
	s32 file_ret;
	char anon_output[OUTPUT_SIZE];
	char file_output[OUTPUT_SIZE];
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct bpf_copy_state);
} state_map SEC(".maps");

SEC("iter.s/task")
int copy_remote_strings(struct bpf_iter__task *ctx)
{
	struct task_struct *task = ctx->task;
	struct bpf_copy_state *state;
	u32 key = 0;

	state = bpf_map_lookup_elem(&state_map, &key);
	if (!state || !task || state->seen || task->pid != state->target_pid)
		return 0;

	state->anon_ret =
		bpf_copy_from_user_task_str(state->anon_output,
					    sizeof(state->anon_output),
					    (void *)state->anon_ptr, task,
					    BPF_F_PAD_ZEROS);
	state->file_ret =
		bpf_copy_from_user_task_str(state->file_output,
					    sizeof(state->file_output),
					    (void *)state->file_ptr, task,
					    BPF_F_PAD_ZEROS);
	state->seen = 1;
	return 0;
}
