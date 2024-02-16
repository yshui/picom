#include <string.h>
#include <xcb/xcb.h>

#include "atom.h"
#include "cache.h"
#include "common.h"
#include "compiler.h"
#include "log.h"
#include "utils.h"

static inline int atom_getter(struct cache *cache attr_unused, const char *atom_name,
                              struct cache_handle **value, void *user_data) {
	xcb_connection_t *c = user_data;
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
	*value = &entry->entry;
	return 0;
}

static inline void
atom_entry_free(struct cache *cache attr_unused, struct cache_handle *handle) {
	struct atom_entry *entry = cache_entry(handle, struct atom_entry, entry);
	free(entry);
}

xcb_atom_t get_atom(struct atom *a, const char *key, xcb_connection_t *c) {
	struct cache_handle *entry = NULL;
	if (cache_get_or_fetch(&a->c, key, &entry, c, atom_getter) < 0) {
		log_error("Failed to get atom %s", key);
		return XCB_NONE;
	}
	return cache_entry(entry, struct atom_entry, entry)->atom;
}

/**
 * Create a new atom structure and fetch all predefined atoms
 */
struct atom *init_atoms(xcb_connection_t *c) {
	auto atoms = ccalloc(1, struct atom);
	atoms->c = CACHE_INIT;
#define ATOM_GET(x) atoms->a##x = get_atom(atoms, #x, c)
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST2);
#undef ATOM_GET
	return atoms;
}

void destroy_atoms(struct atom *a) {
	cache_invalidate_all(&a->c, atom_entry_free);
	free(a);
}
