// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <string.h>
#include <uthash.h>
#include <xcb/xcb.h>

#include "atom.h"
#include "compiler.h"
#include "log.h"
#include "utils/cache.h"
#include "utils/misc.h"

struct atom_entry {
	struct cache_handle entry;
	UT_hash_handle hh;
	xcb_atom_t atom;
};

struct atom_impl {
	struct atom base;
	struct cache c;
	struct atom_entry *atom_to_name;
	cache_getter_t getter;
};

static inline int atom_getter(struct cache *cache, const char *atom_name, size_t keylen,
                              struct cache_handle **value, void *user_data) {
	xcb_connection_t *c = user_data;
	auto atoms = container_of(cache, struct atom_impl, c);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
	    c, xcb_intern_atom(c, 0, to_u16_checked(keylen), atom_name), NULL);

	xcb_atom_t atom = XCB_NONE;
	if (reply) {
		log_debug("Atom %.*s is %d", (int)keylen, atom_name, reply->atom);
		atom = reply->atom;
		free(reply);
	} else {
		log_error("Failed to intern atoms");
		return -1;
	}

	struct atom_entry *entry = ccalloc(1, struct atom_entry);
	entry->atom = atom;
	HASH_ADD_INT(atoms->atom_to_name, atom, entry);
	*value = &entry->entry;
	return 0;
}

static inline int
known_atom_getter(struct cache *cache attr_unused, const char *atom_name attr_unused,
                  size_t keylen attr_unused, struct cache_handle **value, void *user_data) {
	auto atom = *(xcb_atom_t *)user_data;
	struct atom_entry *entry = ccalloc(1, struct atom_entry);
	entry->atom = atom;
	*value = &entry->entry;
	return 0;
}

static inline void atom_entry_free(struct cache *cache, struct cache_handle *handle) {
	auto entry = cache_entry(handle, struct atom_entry, entry);
	auto atoms = container_of(cache, struct atom_impl, c);
	HASH_DEL(atoms->atom_to_name, entry);
	free(entry);
}

xcb_atom_t get_atom(struct atom *a, const char *key, size_t keylen, xcb_connection_t *c) {
	struct cache_handle *entry = NULL;
	auto atoms = container_of(a, struct atom_impl, base);
	if (cache_get_or_fetch(&atoms->c, key, keylen, &entry, c, atoms->getter) < 0) {
		log_error("Failed to get atom %s", key);
		return XCB_NONE;
	}
	return cache_entry(entry, struct atom_entry, entry)->atom;
}

xcb_atom_t get_atom_cached(struct atom *a, const char *key, size_t keylen) {
	auto atoms = container_of(a, struct atom_impl, base);
	auto entry = cache_get(&atoms->c, key, keylen);
	if (!entry) {
		return XCB_NONE;
	}
	return cache_entry(entry, struct atom_entry, entry)->atom;
}

const char *get_atom_name(struct atom *a, xcb_atom_t atom, xcb_connection_t *c) {
	struct atom_entry *entry = NULL;
	auto atoms = container_of(a, struct atom_impl, base);
	HASH_FIND(hh, atoms->atom_to_name, &atom, sizeof(xcb_atom_t), entry);
	if (!entry) {
		BUG_ON(c == NULL);
		auto r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, atom), NULL);
		if (!r) {
			log_error("Failed to get atom name");
			return NULL;
		}
		char *atom_name = xcb_get_atom_name_name(r);
		auto len = (size_t)xcb_get_atom_name_name_length(r);
		struct cache_handle *handle = NULL;
		cache_get_or_fetch(&atoms->c, atom_name, len, &handle, &atom, known_atom_getter);
		entry = cache_entry(handle, struct atom_entry, entry);
		HASH_ADD_INT(atoms->atom_to_name, atom, entry);
		free(r);
	}
	return entry->entry.key;
}

const char *get_atom_name_cached(struct atom *a, xcb_atom_t atom) {
	struct atom_entry *entry = NULL;
	auto atoms = container_of(a, struct atom_impl, base);
	HASH_FIND(hh, atoms->atom_to_name, &atom, sizeof(xcb_atom_t), entry);
	if (!entry) {
		return NULL;
	}
	return entry->entry.key;
}

static inline struct atom *init_atoms_impl(xcb_connection_t *c, cache_getter_t getter) {
	auto atoms = ccalloc(1, struct atom_impl);
	atoms->c = CACHE_INIT;
	atoms->getter = getter;
#define ATOM_GET(x) atoms->base.a##x = get_atom(&atoms->base, #x, sizeof(#x) - 1, c)
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST2);
#undef ATOM_GET
	return &atoms->base;
}

/**
 * Create a new atom structure and fetch all predefined atoms
 */
struct atom *init_atoms(xcb_connection_t *c) {
	return init_atoms_impl(c, atom_getter);
}

void destroy_atoms(struct atom *a) {
	auto atoms = container_of(a, struct atom_impl, base);
	cache_invalidate_all(&atoms->c, atom_entry_free);
	assert(atoms->atom_to_name == NULL);
	free(a);
}

#if defined(UNIT_TEST) || defined(CONFIG_FUZZER)

static inline int mock_atom_getter(struct cache *cache, const char *atom_name attr_unused,
                                   size_t atom_len attr_unused, struct cache_handle **value,
                                   void *user_data attr_unused) {
	auto atoms = container_of(cache, struct atom_impl, c);
	xcb_atom_t atom = (xcb_atom_t)HASH_COUNT(atoms->atom_to_name) + 1;
	struct atom_entry *entry = ccalloc(1, struct atom_entry);
	entry->atom = atom;
	HASH_ADD_INT(atoms->atom_to_name, atom, entry);
	*value = &entry->entry;
	return 0;
}

struct atom *init_mock_atoms(void) {
	return init_atoms_impl(NULL, mock_atom_getter);
}

#else

struct atom *init_mock_atoms(void) {
	abort();
}

#endif
