// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>

struct list_node {
	struct list_node *next, *prev;
};

#define list_entry(ptr, type, node) container_of(ptr, type, node)
#define list_next_entry(ptr, node) list_entry((ptr)->node.next, __typeof__(*(ptr)), node)
#define list_prev_entry(ptr, node) list_entry((ptr)->node.prev, __typeof__(*(ptr)), node)

/// Insert a new node between two adjacent nodes in the list
static inline void __list_insert_between(struct list_node *prev, struct list_node *next,
                                         struct list_node *new_) {
	new_->prev = prev;
	new_->next = next;
	next->prev = new_;
	prev->next = new_;
}

/// Insert a new node after `curr`
static inline void list_insert_after(struct list_node *curr, struct list_node *new_) {
	__list_insert_between(curr, curr->next, new_);
}

/// Insert a new node before `curr`
static inline void list_insert_before(struct list_node *curr, struct list_node *new_) {
	__list_insert_between(curr->prev, curr, new_);
}

/// Link two nodes in the list, so `next` becomes the successor node of `prev`
static inline void __list_link(struct list_node *prev, struct list_node *next) {
	next->prev = prev;
	prev->next = next;
}

/// Remove a node from the list
static inline void list_remove(struct list_node *to_remove) {
	__list_link(to_remove->prev, to_remove->next);
	to_remove->prev = (void *)-1;
	to_remove->next = (void *)-2;
}

/// Move `to_move` so that it's before `new_next`
static inline void list_move_before(struct list_node *to_move, struct list_node *new_next) {
	list_remove(to_move);
	list_insert_before(new_next, to_move);
}

/// Move `to_move` so that it's after `new_prev`
static inline void list_move_after(struct list_node *to_move, struct list_node *new_prev) {
	list_remove(to_move);
	list_insert_after(new_prev, to_move);
}

/// Initialize a list node that's intended to be the head node
static inline void list_init_head(struct list_node *head) {
	head->next = head->prev = head;
}

/// Replace list node `old` with `n`
static inline void list_replace(struct list_node *old, struct list_node *n) {
	__list_insert_between(old->prev, old->next, n);
	old->prev = (void *)-1;
	old->next = (void *)-2;
}

/// Return true if head is the only node in the list. Under usual circumstances this means
/// the list is empty
static inline bool list_is_empty(const struct list_node *head) {
	return head->prev == head;
}

/// Splice a list of nodes from `from` to into the beginning of list `to`.
static inline void list_splice(struct list_node *from, struct list_node *to) {
	if (list_is_empty(from)) {
		return;
	}
	__list_link(from->prev, to->next);
	__list_link(to, from->next);
	list_init_head(from);
}

/// Return true if `to_check` is the first node in list headed by `head`
static inline bool
list_node_is_first(const struct list_node *head, const struct list_node *to_check) {
	return head->next == to_check;
}

/// Return true if `to_check` is the last node in list headed by `head`
static inline bool
list_node_is_last(const struct list_node *head, const struct list_node *to_check) {
	return head->prev == to_check;
}

#define list_foreach(type, i, head, member)                                              \
	for (type *i = list_entry((head)->next, type, member); &i->member != (head);     \
	     i = list_next_entry(i, member))

/// Like list_for_each, but it's safe to remove the current list node from the list
#define list_foreach_safe(type, i, head, member)                                         \
	for (type *i = list_entry((head)->next, type, member),                           \
	          *__tmp = list_next_entry(i, member);                                   \
	     &i->member != (head); i = __tmp, __tmp = list_next_entry(i, member))
