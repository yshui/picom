#pragma once
#include <stdbool.h>
#include <X11/Xlib.h>
typedef struct session session_t;
typedef struct winprop winprop_t;
/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param ps current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
winprop_t
wid_get_prop_adv(const session_t *ps, Window w, Atom atom, long offset,
    long length, Atom rtype, int rformat);

/**
 * Get the value of a type-<code>Window</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop);

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr);
