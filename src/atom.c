#include <string.h>
#include <xcb/xcb.h>

#include "atom.h"
#include "common.h"
#include "log.h"
#include "utils.h"

static inline void *atom_getter(void *ud, const char *atom_name, int *err) {
	xcb_connection_t *c = ud;
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
	    c, xcb_intern_atom(c, 0, to_u16_checked(strlen(atom_name)), atom_name), NULL);

	xcb_atom_t atom = XCB_NONE;
	if (reply) {
		log_debug("Atom %s is %d", atom_name, reply->atom);
		atom = reply->atom;
		free(reply);
	} else {
		log_error("Failed to intern atoms");
		*err = 1;
	}
	return (void *)(intptr_t)atom;
}

/**
 * Create a new atom structure and fetch all predefined atoms
 */
struct atom *init_atoms(xcb_connection_t *c) {
	auto atoms = ccalloc(1, struct atom);
	atoms->c = new_cache((void *)c, atom_getter, NULL);
#define ATOM_GET(x) atoms->a##x = (xcb_atom_t)(intptr_t)cache_get(atoms->c, #x, NULL)
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_GET, SEP_COLON, ATOM_LIST2);
#undef ATOM_GET
	return atoms;
}
