// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2020 Facebook */
// clang-format off
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
// clang-format on

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct xcb_connection_t {
	/* This must be the first field; see _xcb_conn_ret_error(). */
	int has_error;

	/* constant data */
	void *setup;
	int fd;
};
struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(u32));
	__uint(max_entries, 2);
} events SEC(".maps");

struct event {
	u8 task[TASK_COMM_LEN];
	__u64 delta_us;
	pid_t pid;
};

struct event _event = {0};
u64 pid = 0;
void *conn_ptr;
char last_stack[4096];
u64 last_recv_stack_size;

SEC("uprobe")
int BPF_KPROBE(uprobe_recvmsg, const char *trace, u64 size) {
	if (pid != bpf_get_current_pid_tgid() >> 32) {
		return 0;
	}
	last_recv_stack_size = 0;
	if (size > 4096) {
		bpf_printk("invalid stack size %u", size);
		return 0;
	}
	if (bpf_probe_read_user(&last_stack[0], size, trace)) {
		bpf_printk("cannot read");
		return 0;
	}
	last_recv_stack_size = size;
}

SEC("uprobe")
int BPF_KPROBE(uprobe_epoll_wait) {
	if (pid != bpf_get_current_pid_tgid() >> 32) {
		return 0;
	}
	struct xcb_connection_t conn;
	if (bpf_probe_read_user(&conn, sizeof(conn), conn_ptr)) {
		bpf_printk("cannot read");
		return 0;
	}
	u32 queue_len;
	if (bpf_probe_read_user(&queue_len, sizeof(queue_len), conn_ptr + 4212)) {
		bpf_printk("cannot read");
		return 0;
	}
	u64 event_head;
	if (bpf_probe_read_user(&event_head, sizeof(event_head), conn_ptr + 4272)) {
		bpf_printk("cannot read");
		return 0;
	}

	if (event_head != 0 || queue_len != 0) {
		bpf_printk("epoll_wait %d %p %d", conn.fd, event_head, queue_len);
		char data[16];
		*(u64 *)data = event_head;
		*(u64 *)(data + 8) = (u64)queue_len;
		bpf_perf_event_output(ctx, &events, 1, data, 16);
		if (last_recv_stack_size <= 4096) {
			bpf_perf_event_output(ctx, &events, 0, last_stack, last_recv_stack_size);
		}
	}
	return 0;
}

SEC("uprobe")
int BPF_KPROBE(uprobe_xcb_conn, void *ptr) {
	struct xcb_connection_t conn;
	pid = bpf_get_current_pid_tgid() >> 32;
	bpf_printk("xcb connection is %p", ptr);
	if (bpf_probe_read_user(&conn, sizeof(conn), ptr)) {
		bpf_printk("cannot read");
	} else {
		bpf_printk("fd is %d", conn.fd);
		conn_ptr = ptr;
	}
	return 0;
}
