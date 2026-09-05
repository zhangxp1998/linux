// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define OUTPUT_SIZE 32

char _license[] SEC("license") = "GPL";

extern int bpf_copy_from_user_task_str(void *dst, u32 dst__sz,
				       const void *unsafe_ptr__ign,
				       struct task_struct *tsk,
				       u64 flags) __ksym;

struct bpf_copy_state {
	u32 target_pid;
	u32 seen;
	u64 anon_ptr;
	u64 file_ptr;
	s32 anon_ret;
	s32 file_ret;
	s32 invalid_flags_ret;
	s32 zero_size_ret;
	s32 fault_pad_ret;
	s32 fault_no_pad_ret;
	s32 no_pad_ret;
	char anon_output[OUTPUT_SIZE];
	char file_output[OUTPUT_SIZE];
	char fault_pad_output[8];
	char no_pad_output[OUTPUT_SIZE];
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

	state->invalid_flags_ret =
		bpf_copy_from_user_task_str(state->anon_output,
					    sizeof(state->anon_output),
					    (void *)state->anon_ptr, task,
					    BPF_F_PAD_ZEROS | (1ULL << 63));
	state->zero_size_ret =
		bpf_copy_from_user_task_str(state->anon_output, 0,
					    (void *)state->anon_ptr, task, 0);
	__builtin_memset(state->fault_pad_output, 0x5a,
			 sizeof(state->fault_pad_output));
	state->fault_pad_ret =
		bpf_copy_from_user_task_str(state->fault_pad_output,
					    sizeof(state->fault_pad_output),
					    (void *)1, task, BPF_F_PAD_ZEROS);
	state->fault_no_pad_ret =
		bpf_copy_from_user_task_str(state->fault_pad_output,
					    sizeof(state->fault_pad_output),
					    (void *)1, task, 0);
	state->no_pad_ret =
		bpf_copy_from_user_task_str(state->no_pad_output,
					    sizeof(state->no_pad_output),
					    (void *)state->anon_ptr, task, 0);
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
