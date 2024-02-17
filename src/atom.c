#include <string.h>
#include <uthash.h>
#include <xcb/xcb.h>

#include "atom.h"
#include "cache.h"
#include "common.h"
#include "compiler.h"
#include "log.h"
#include "utils.h"

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
	char *(*name_getter)(xcb_atom_t atom, xcb_connection_t *c);
};

static inline int atom_getter(struct cache *cache, const char *atom_name,
                              struct cache_handle **value, void *user_data) {
	xcb_connection_t *c = user_data;
	auto atoms = container_of(cache, struct atom_impl, c);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
	    c, xcb_intern_atom(c, 0, to_u16_checked(strlen(atom_name)), atom_name), NULL);

	xcb_atom_t atom = XCB_NONE;
	if (reply) {
		log_debug("Atom %s is %d", atom_name, reply->atom);
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

static inline char *atom_name_getter(xcb_atom_t atom, xcb_connection_t *c) {
	auto r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, atom), NULL);
	if (!r) {
		log_error("Failed to get atom name");
		return NULL;
	}
	char *atom_name = strdup(xcb_get_atom_name_name(r));
	free(r);
	return atom_name;
}

static inline int
known_atom_getter(struct cache *cache attr_unused, const char *atom_name attr_unused,
                  struct cache_handle **value, void *user_data) {
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

xcb_atom_t get_atom(struct atom *a, const char *key, xcb_connection_t *c) {
	struct cache_handle *entry = NULL;
	auto atoms = container_of(a, struct atom_impl, base);
	if (cache_get_or_fetch(&atoms->c, key, &entry, c, atoms->getter) < 0) {
		log_error("Failed to get atom %s", key);
		return XCB_NONE;
	}
	return cache_entry(entry, struct atom_entry, entry)->atom;
}

xcb_atom_t get_atom_cached(struct atom *a, const char *key) {
	auto atoms = container_of(a, struct atom_impl, base);
	return cache_entry(cache_get(&atoms->c, key), struct atom_entry, entry)->atom;
}

const char *get_atom_name(struct atom *a, xcb_atom_t atom, xcb_connection_t *c) {
	struct atom_entry *entry = NULL;
	auto atoms = container_of(a, struct atom_impl, base);
	HASH_FIND(hh, atoms->atom_to_name, &atom, sizeof(xcb_atom_t), entry);
	if (!entry) {
		char *atom_name = atoms->name_getter(atom, c);
		if (!atom_name) {
			return NULL;
		}
		struct cache_handle *handle = NULL;
		cache_get_or_fetch(&atoms->c, atom_name, &handle, &atom, known_atom_getter);
		entry = cache_entry(handle, struct atom_entry, entry);
		free(atom_name);
	}
	HASH_ADD_INT(atoms->atom_to_name, atom, entry);
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

/**
 * Create a new atom structure and fetch all predefined atoms
 */
struct atom *init_atoms(xcb_connection_t *c) {
	auto atoms = ccalloc(1, struct atom_impl);
	atoms->c = CACHE_INIT;
	atoms->getter = atom_getter;
	atoms->name_getter = atom_name_getter;
#define ATOM_GET(x) atoms->base.a##x = get_atom(&atoms->base, #x, c)
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST2);
#undef ATOM_GET
	return &atoms->base;
}

void destroy_atoms(struct atom *a) {
	auto atoms = container_of(a, struct atom_impl, base);
	cache_invalidate_all(&atoms->c, atom_entry_free);
	free(a);
}
