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
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, u32);
	__type(value, u64);
	__uint(max_entries, 256);
} my_map SEC(".maps");

struct event {
	u8 task[TASK_COMM_LEN];
	__u64 delta_us;
	pid_t pid;
};

struct event _event = {0};

SEC("uprobe")
int BPF_KPROBE(uprobe_epoll_wait) {
	u64 *curr_pid = bpf_map_lookup_elem(&my_map, (u32[]){0});
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if (!curr_pid || pid != *curr_pid) {
		return 0;
	}
	void **conn_ptr = bpf_map_lookup_elem(&my_map, (u32[]){1});
	if (!conn_ptr) {
		return 0;
	}
	struct xcb_connection_t conn;
	if (bpf_probe_read_user(&conn, sizeof(conn), *conn_ptr)) {
		bpf_printk("cannot read");
		return 0;
	}
#if 0
	u64 request_read;
	if (bpf_probe_read_user(&request_read, sizeof(request_read), (*conn_ptr) + 4216)) {
		bpf_printk("cannot read");
		return 0;
	}
	bpf_printk("request read %x", request_read);
#endif
	u64 event_head;
	if (bpf_probe_read_user(&event_head, sizeof(event_head), (*conn_ptr) + 4272)) {
		bpf_printk("cannot read");
		return 0;
	}

	if (event_head != 0) {
		bpf_printk("epoll_wait %d %p", conn.fd, event_head);
	}
	return 0;
}

SEC("uprobe")
int BPF_KPROBE(uprobe_xcb_conn, void *ptr) {
	struct xcb_connection_t conn;
	u64 pid = bpf_get_current_pid_tgid() >> 32;
	bpf_map_update_elem(&my_map, (u32[]){0}, &pid, 0);
	bpf_printk("xcb connection is %p", ptr);
	if (bpf_probe_read_user(&conn, sizeof(conn), ptr)) {
		bpf_printk("cannot read");
	} else {
		bpf_map_update_elem(&my_map, (u32[]){1}, &ptr, 0);
		bpf_printk("fd is %d", conn.fd);
	}
	return 0;
}
