// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <ctype.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uthash.h>

// libpcre
#ifdef CONFIG_REGEX_PCRE
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#endif

#include <X11/Xlib.h>
#include <xcb/xcb.h>

#include "atom.h"
#include "common.h"
#include "compiler.h"
#include "log.h"
#include "test.h"
#include "utils/str.h"
#include "utils/uthash_extra.h"
#include "wm/win.h"
#include "x.h"

#include "c2.h"

#pragma GCC diagnostic error "-Wunused-parameter"

#define C2_MAX_LEVELS 10

typedef struct c2_condition_node_branch c2_condition_node_branch;
typedef struct c2_condition_node_leaf c2_condition_node_leaf;

enum c2_condition_node_type {
	C2_NODE_TYPE_BRANCH,
	C2_NODE_TYPE_LEAF,
	C2_NODE_TYPE_TRUE,
};

/// Fat, typed pointer to a condition tree node.
typedef struct {
	enum c2_condition_node_type type;
	union {
		struct c2_condition_node_branch *b;
		struct c2_condition_node_leaf *l;
	};
	bool neg;
} c2_condition_node_ptr;

struct c2_tracked_property_key {
	xcb_atom_t property;
	bool is_on_client;
	char padding[3];
};
static_assert(sizeof(struct c2_tracked_property_key) == 8, "Padding bytes in "
                                                           "c2_tracked_property_key");

struct c2_tracked_property {
	UT_hash_handle hh;
	struct c2_tracked_property_key key;
	unsigned int id;
	/// Highest indices of this property that
	/// are tracked. -1 mean all indices are tracked.
	int max_indices;
};

struct c2_state {
	struct c2_tracked_property *tracked_properties;
	struct atom *atoms;
	xcb_get_property_cookie_t *cookies;
	uint32_t *propert_lengths;
};

// TODO(yshui) this has some overlap with winprop_t, consider merging them.
struct c2_property_value {
	union {
		struct {
			char *string;
		};
		struct {
			int64_t numbers[4];
		};
		struct {
			int64_t *array;
			unsigned int capacity;
		};
	};
	/// Number of items if the property is a number type,
	/// or number of bytes in the string if the property is a string type.
	uint32_t length;
	enum {
		C2_PROPERTY_TYPE_STRING,
		C2_PROPERTY_TYPE_NUMBER,
		C2_PROPERTY_TYPE_ATOM,
	} type;
	bool valid;
	bool needs_update;
};

/// Initializer for c2_ptr_t.
static const c2_condition_node_ptr C2_NODE_PTR_INIT = {
    .type = C2_NODE_TYPE_LEAF,
    .l = NULL,
};

/// Operator of a branch element.
typedef enum {
	C2_B_OUNDEFINED,
	C2_B_OAND,
	C2_B_OOR,
	C2_B_OXOR,
} c2_b_op_t;

/// Structure for branch element in a window condition
struct c2_condition_node_branch {
	c2_b_op_t op;
	c2_condition_node_ptr opr1;
	c2_condition_node_ptr opr2;
};

/// Initializer for c2_b_t.
#define C2_B_INIT                                                                        \
	{.neg = false, .op = C2_B_OUNDEFINED, .opr1 = C2_PTR_INIT, .opr2 = C2_PTR_INIT}

/// Structure for leaf element in a window condition
struct c2_condition_node_leaf {
	enum {
		C2_L_OEXISTS = 0,
		C2_L_OEQ,
		C2_L_OGT,
		C2_L_OGTEQ,
		C2_L_OLT,
		C2_L_OLTEQ,
	} op : 3;
	enum {
		C2_L_MEXACT,
		C2_L_MSTART,
		C2_L_MCONTAINS,
		C2_L_MWILDCARD,
		C2_L_MPCRE,
	} match : 3;
	bool match_ignorecase : 1;
	char *tgt;
	unsigned int target_id;
	xcb_atom_t tgtatom;
	bool target_on_client;
	int index;
	// TODO(yshui) translate some of the pre-defined targets to
	//             generic window properties. e.g. `name = "xterm"`
	//             should be translated to:
	//               "WM_NAME = 'xterm' || _NET_WM_NAME = 'xterm'"
	enum {
		C2_L_PUNDEFINED = -1,
		C2_L_PID = 0,
		C2_L_PX,
		C2_L_PY,
		C2_L_PX2,
		C2_L_PY2,
		C2_L_PWIDTH,
		C2_L_PHEIGHT,
		C2_L_PWIDTHB,
		C2_L_PHEIGHTB,
		C2_L_PBDW,
		C2_L_PFULLSCREEN,
		C2_L_POVREDIR,
		C2_L_PARGB,
		C2_L_PFOCUSED,
		C2_L_PGROUPFOCUSED,
		C2_L_PWMWIN,
		C2_L_PBSHAPED,
		C2_L_PROUNDED,
		C2_L_PCLIENT,
		C2_L_PWINDOWTYPE,
		C2_L_PLEADER,
		C2_L_PNAME,
		C2_L_PCLASSG,
		C2_L_PCLASSI,
		C2_L_PROLE,
	} predef;
	enum {
		C2_L_PTUNDEFINED,
		C2_L_PTSTRING,
		C2_L_PTINT,
	} ptntype;
	char *ptnstr;
	long ptnint;
#ifdef CONFIG_REGEX_PCRE
	pcre2_code *regex_pcre;
	pcre2_match_data *regex_pcre_match;
#endif
};

static const unsigned int C2_L_INVALID_TARGET_ID = UINT_MAX;
/// Initializer for c2_l_t.
static const c2_condition_node_leaf C2_LEAF_NODE_INIT = {
    .op = C2_L_OEXISTS,
    .match = C2_L_MEXACT,
    .match_ignorecase = false,
    .tgt = NULL,
    .tgtatom = 0,
    .target_on_client = false,
    .predef = C2_L_PUNDEFINED,
    .index = 0,
    .ptntype = C2_L_PTUNDEFINED,
    .ptnstr = NULL,
    .ptnint = 0,
    .target_id = C2_L_INVALID_TARGET_ID,
};

/// Linked list type of conditions.
struct c2_condition {
	c2_condition_node_ptr root;
	void *data;
	struct list_node siblings;
};

// clang-format off
// Predefined targets.
static const struct {
	const char *name;
	bool is_string;
	bool deprecated;
} C2_PREDEFS[] = {
    [C2_L_PID]           = { "id", false, true },
    [C2_L_PX]            = { "x", false, },
    [C2_L_PY]            = { "y", false, },
    [C2_L_PX2]           = { "x2", false, },
    [C2_L_PY2]           = { "y2", false, },
    [C2_L_PWIDTH]        = { "width", false, },
    [C2_L_PHEIGHT]       = { "height", false, },
    [C2_L_PWIDTHB]       = { "widthb", false, },
    [C2_L_PHEIGHTB]      = { "heightb", false, },
    [C2_L_PBDW]          = { "border_width", false, },
    [C2_L_PFULLSCREEN]   = { "fullscreen", false, },
    [C2_L_POVREDIR]      = { "override_redirect", false, },
    [C2_L_PARGB]         = { "argb", false, },
    [C2_L_PFOCUSED]      = { "focused", false, },
    [C2_L_PGROUPFOCUSED] = { "group_focused", false, },
    [C2_L_PWMWIN]        = { "wmwin", false, },
    [C2_L_PBSHAPED]      = { "bounding_shaped", false, },
    [C2_L_PROUNDED]      = { "rounded_corners", false, },
    [C2_L_PCLIENT]       = { "client", false, true },
    [C2_L_PWINDOWTYPE]   = { "window_type", true, },
    [C2_L_PLEADER]       = { "leader", false, true },
    [C2_L_PNAME]         = { "name", true, },
    [C2_L_PCLASSG]       = { "class_g", true, },
    [C2_L_PCLASSI]       = { "class_i", true, },
    [C2_L_PROLE]         = { "role", true, },
};
// clang-format on

static const char *const c2_pattern_type_names[] = {
    [C2_L_PTUNDEFINED] = "undefined",
    [C2_L_PTSTRING] = "string",
    [C2_L_PTINT] = "number",
};

/**
 * Compare next word in a string with another string.
 */
static inline int strcmp_wd(const char *needle, const char *src) {
	int ret = mstrncmp(needle, src);
	if (ret) {
		return ret;
	}

	char c = src[strlen(needle)];
	if (isalnum((unsigned char)c) || '_' == c) {
		return 1;
	}

	return 0;
}

/**
 * Combine two condition trees.
 */
static inline c2_condition_node_ptr
c2h_comb_tree(c2_b_op_t op, c2_condition_node_ptr p1, c2_condition_node_ptr p2) {
	c2_condition_node_ptr p = {.type = C2_NODE_TYPE_BRANCH, .b = NULL};
	p.b = cmalloc(struct c2_condition_node_branch);

	p.b->op = op;
	p.b->opr1 = p1;
	p.b->opr2 = p2;

	return p;
}

/**
 * Get the precedence value of a condition branch operator.
 */
static inline int c2h_b_opp(c2_b_op_t op) {
	switch (op) {
	case C2_B_OAND: return 2;
	case C2_B_OOR:
	case C2_B_OXOR: return 1;
	default: break;
	}

	assert(0);
	return 0;
}

/**
 * Compare precedence of two condition branch operators.
 *
 * Associativity is left-to-right, forever.
 *
 * @return positive number if op1 > op2, 0 if op1 == op2 in precedence,
 *         negative number otherwise
 */
static inline int c2h_b_opcmp(c2_b_op_t op1, c2_b_op_t op2) {
	return c2h_b_opp(op1) - c2h_b_opp(op2);
}

static int
c2_parse_group(const char *pattern, int offset, c2_condition_node_ptr *presult, int level);
static int c2_parse_target(const char *pattern, int offset, c2_condition_node_ptr *presult);
static int c2_parse_op(const char *pattern, int offset, c2_condition_node_ptr *presult);
static int c2_parse_pattern(const char *pattern, int offset, c2_condition_node_ptr *presult);
static int c2_parse_legacy(const char *pattern, int offset, c2_condition_node_ptr *presult);
static void c2_free(c2_condition_node_ptr p);
static size_t c2_condition_node_to_str(c2_condition_node_ptr p, char *output, size_t len);
static const char *c2_condition_node_to_str2(c2_condition_node_ptr ptr);
static bool
c2_tree_postprocess(struct c2_state *state, xcb_connection_t *c, c2_condition_node_ptr node);

/**
 * Wrapper of c2_free().
 */
static inline void c2_freep(c2_condition_node_ptr *pp) {
	if (pp) {
		c2_free(*pp);
		*pp = C2_NODE_PTR_INIT;
	}
}

/**
 * Parse a condition string.
 */
struct c2_condition *c2_parse(struct list_node *list, const char *pattern, void *data) {
	if (!pattern) {
		return NULL;
	}

	// Parse the pattern
	auto result = C2_NODE_PTR_INIT;
	int offset = -1;

	if (strlen(pattern) >= 2 && ':' == pattern[1]) {
		offset = c2_parse_legacy(pattern, 0, &result);
	} else {
		offset = c2_parse_group(pattern, 0, &result, 0);
	}

	if (offset < 0) {
		c2_freep(&result);
		return NULL;
	}

	// Insert to pcondlst
	{
		auto plptr = cmalloc(struct c2_condition);
		*plptr = (struct c2_condition){
		    .root = result,
		    .data = data,
		};
		list_init_head(&plptr->siblings);
		if (list) {
			list_insert_after(list, &plptr->siblings);
		}

#ifdef DEBUG_C2
		log_trace("(\"%s\"): ", pattern);
		c2_dump(plptr->ptr);
		putchar('\n');
#endif

		return plptr;
	}
}

/**
 * Parse a condition string with a prefix.
 */
c2_condition *
c2_parse_with_prefix(struct list_node *list, const char *pattern,
                     void *(*parse_prefix)(const char *input, const char **end, void *),
                     void (*free_value)(void *), void *user_data) {
	char *pattern_start = NULL;
	void *val = parse_prefix(pattern, (const char **)&pattern_start, user_data);
	if (pattern_start == NULL) {
		return NULL;
	}
	auto ret = c2_parse(list, pattern_start, val);
	if (!ret && free_value) {
		free_value(val);
	}
	return ret;
}

TEST_CASE(c2_parse) {
	char str[1024];
	struct c2_condition *cond = c2_parse(NULL, "name = \"xterm\"", NULL);
	struct atom *atoms = init_mock_atoms();
	struct c2_state *state = c2_state_new(atoms);
	TEST_NOTEQUAL(cond, NULL);
	TEST_EQUAL(!cond->root.type, C2_NODE_TYPE_BRANCH);
	TEST_NOTEQUAL(cond->root.l, NULL);
	TEST_EQUAL(cond->root.l->op, C2_L_OEQ);
	TEST_EQUAL(cond->root.l->ptntype, C2_L_PTSTRING);
	TEST_STREQUAL(cond->root.l->ptnstr, "xterm");

	size_t len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "name = \"xterm\"", len);

	struct wm *wm = wm_new();
	struct wm_ref *node = wm_new_mock_window(wm, 1);

	struct win test_win = {
	    .name = "xterm",
	    .tree_ref = node,
	};
	TEST_TRUE(c2_match_one(state, &test_win, cond, NULL));
	c2_tree_postprocess(state, NULL, cond->root);
	TEST_EQUAL(HASH_COUNT(state->tracked_properties), 0);
	c2_state_free(state);
	destroy_atoms(atoms);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "argb", NULL);
	TEST_NOTEQUAL(cond, NULL);
	TEST_NOTEQUAL(cond->root.type, C2_NODE_TYPE_BRANCH);
	TEST_EQUAL(cond->root.l->ptntype, C2_L_PTINT);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "argb = 'b'", NULL);
	TEST_EQUAL(cond, NULL);

	cond = c2_parse(NULL, "_GTK_FRAME_EXTENTS@:c", NULL);
	TEST_NOTEQUAL(cond, NULL);
	TEST_NOTEQUAL(cond->root.type, C2_NODE_TYPE_BRANCH);
	TEST_NOTEQUAL(cond->root.l, NULL);
	TEST_EQUAL(cond->root.l->op, C2_L_OEXISTS);
	TEST_EQUAL(cond->root.l->match, C2_L_MEXACT);
	TEST_EQUAL(cond->root.l->predef, C2_L_PUNDEFINED);
	TEST_TRUE(cond->root.l->target_on_client);
	TEST_NOTEQUAL(cond->root.l->tgt, NULL);
	TEST_STREQUAL(cond->root.l->tgt, "_GTK_FRAME_EXTENTS");

	atoms = init_mock_atoms();
	state = c2_state_new(atoms);
	c2_tree_postprocess(state, NULL, cond->root);
	TEST_EQUAL(HASH_COUNT(state->tracked_properties), 1);
	HASH_ITER2(state->tracked_properties, prop) {
		TEST_EQUAL(prop->key.property,
		           get_atom_with_nul(state->atoms, "_GTK_FRAME_EXTENTS", NULL));
		TEST_EQUAL(prop->max_indices, 0);
	}
	c2_state_free(state);
	destroy_atoms(atoms);

	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "_GTK_FRAME_EXTENTS@[0]", len);
	c2_free_condition(cond, NULL);

	cond = c2_parse(
	    NULL, "!(name != \"xterm\" && class_g *= \"XTerm\") || !name != \"yterm\"", NULL);
	TEST_NOTEQUAL(cond, NULL);
	TEST_STREQUAL(c2_condition_to_str(cond), "(!(name != \"xterm\" && class_g *= "
	                                         "\"XTerm\") || name = \"yterm\")");
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "name = \"xterm\" && class_g *= \"XTerm\"", NULL);
	TEST_NOTEQUAL(cond, NULL);
	TEST_EQUAL(cond->root.type, C2_NODE_TYPE_BRANCH);
	TEST_NOTEQUAL(cond->root.b, NULL);
	TEST_EQUAL(cond->root.b->op, C2_B_OAND);
	TEST_NOTEQUAL(cond->root.b->opr1.l, NULL);
	TEST_NOTEQUAL(cond->root.b->opr2.l, NULL);
	TEST_EQUAL(cond->root.b->opr1.l->op, C2_L_OEQ);
	TEST_EQUAL(cond->root.b->opr1.l->match, C2_L_MEXACT);
	TEST_EQUAL(cond->root.b->opr1.l->ptntype, C2_L_PTSTRING);
	TEST_EQUAL(cond->root.b->opr2.l->op, C2_L_OEQ);
	TEST_EQUAL(cond->root.b->opr2.l->match, C2_L_MCONTAINS);
	TEST_EQUAL(cond->root.b->opr2.l->ptntype, C2_L_PTSTRING);
	TEST_STREQUAL(cond->root.b->opr1.l->tgt, "name");
	TEST_EQUAL(cond->root.b->opr1.l->predef, C2_L_PNAME);
	TEST_STREQUAL(cond->root.b->opr2.l->tgt, "class_g");
	TEST_EQUAL(cond->root.b->opr2.l->predef, C2_L_PCLASSG);

	atoms = init_mock_atoms();
	state = c2_state_new(atoms);
	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "(name = \"xterm\" && class_g *= \"XTerm\")", len);
	test_win.class_general = "XTerm";
	TEST_TRUE(c2_match_one(state, &test_win, cond, NULL));
	test_win.class_general = "asdf";
	TEST_TRUE(!c2_match_one(state, &test_win, cond, NULL));
	c2_free_condition(cond, NULL);
	c2_state_free(state);
	destroy_atoms(atoms);

	cond = c2_parse(NULL, "_NET_WM_STATE[1]:32a *='_NET_WM_STATE_HIDDEN'", NULL);
	TEST_EQUAL(cond->root.l->index, 1);
	TEST_STREQUAL(cond->root.l->tgt, "_NET_WM_STATE");
	TEST_STREQUAL(cond->root.l->ptnstr, "_NET_WM_STATE_HIDDEN");

	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "_NET_WM_STATE[1] *= \"_NET_WM_STATE_HIDDEN\"", len);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "_NET_WM_STATE[*]:32a*='_NET_WM_STATE_HIDDEN'", NULL);
	TEST_EQUAL(cond->root.l->index, -1);

	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "_NET_WM_STATE[*] *= \"_NET_WM_STATE_HIDDEN\"", len);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "!class_i:0s", NULL);
	TEST_NOTEQUAL(cond, NULL);
	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, "!class_i", len);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "_NET_WM_STATE = '_NET_WM_STATE_HIDDEN'", NULL);
	TEST_NOTEQUAL(cond, NULL);
	c2_free_condition(cond, NULL);

	cond = c2_parse(NULL, "1A:\n1111111111111ar1", NULL);
	TEST_EQUAL(cond, NULL);

	cond = c2_parse(NULL, "N [4444444444444: \n", NULL);
	TEST_EQUAL(cond, NULL);

	cond = c2_parse(NULL, " x:a=\"b\377\\xCCCCC", NULL);
	TEST_EQUAL(cond, NULL);

	cond = c2_parse(NULL, "!!!!!!!((((((!(((((,", NULL);
	TEST_EQUAL(cond, NULL);

	const char *rule = "(((role = \"\\\\tg^\\n\\n[\\t\" && role ~?= \"\") && "
	                   "role ~?= \"\\n\\n\\n\\b\\n^\\n*0bon\") && role ~?= "
	                   "\"\\n\\n\\x8a\\b\\n^\\n*0\\n[\\n[\\n\\n\\b\\n\")";
	cond = c2_parse(NULL, rule, NULL);
	TEST_NOTEQUAL(cond, NULL);
	len = c2_condition_node_to_str(cond->root, str, sizeof(str));
	TEST_STREQUAL3(str, rule, len);
	c2_free_condition(cond, NULL);

	wm_free_mock_window(wm, test_win.tree_ref);
	wm_free(wm);
}

#define c2_error(format, ...)                                                                \
	do {                                                                                 \
		log_error("Pattern \"%s\" pos %d: " format, pattern, offset, ##__VA_ARGS__); \
		goto fail;                                                                   \
	} while (0)

// TODO(yshui) Not a very good macro, should probably be a function
#define C2H_SKIP_SPACES()                                                                \
	{                                                                                \
		while (isspace((unsigned char)pattern[offset]))                          \
			++offset;                                                        \
	}

/**
 * Parse a group in condition string.
 *
 * @return offset of next character in string
 */
static int
c2_parse_group(const char *pattern, int offset, c2_condition_node_ptr *presult, int level) {
	if (!pattern) {
		return -1;
	}

	// Expected end character
	const char endchar = (offset ? ')' : '\0');

	// We use a system that a maximum of 2 elements are kept. When we find
	// the third element, we combine the elements according to operator
	// precedence. This design limits operators to have at most two-levels
	// of precedence and fixed left-to-right associativity.

	// For storing branch operators. ops[0] is actually unused
	c2_b_op_t ops[3] = {};
	// For storing elements
	c2_condition_node_ptr eles[2] = {C2_NODE_PTR_INIT, C2_NODE_PTR_INIT};
	// Index of next free element slot in eles
	int elei = 0;
	// Pointer to the position of next element
	c2_condition_node_ptr *pele = eles;
	// Negation flag of next operator
	bool neg = false;
	// Whether we are expecting an element immediately, is true at first, or
	// after encountering a logical operator
	bool next_expected = true;

	// Check for recursion levels
	if (level > C2_MAX_LEVELS) {
		c2_error("Exceeded maximum recursion levels.");
	}

	// Parse the pattern character-by-character
	for (; pattern[offset]; ++offset) {
		assert(elei <= 2);

		// Jump over spaces
		if (isspace((unsigned char)pattern[offset])) {
			continue;
		}

		// Handle end of group
		if (')' == pattern[offset]) {
			break;
		}

		// Handle "!"
		if ('!' == pattern[offset]) {
			if (!next_expected) {
				c2_error("Unexpected \"!\".");
			}

			neg = !neg;
			continue;
		}

		// Handle AND and OR
		if ('&' == pattern[offset] || '|' == pattern[offset]) {
			if (next_expected) {
				c2_error("Unexpected logical operator.");
			}

			next_expected = true;
			if (mstrncmp("&&", pattern + offset) == 0) {
				ops[elei] = C2_B_OAND;
				++offset;
			} else if (mstrncmp("||", pattern + offset) == 0) {
				ops[elei] = C2_B_OOR;
				++offset;
			} else {
				c2_error("Illegal logical operator.");
			}

			continue;
		}

		// Parsing an element
		if (!next_expected) {
			c2_error("Unexpected expression.");
		}

		assert(!elei || ops[elei]);

		// If we are out of space
		if (elei == 2) {
			--elei;
			// If the first operator has higher or equal precedence, combine
			// the first two elements
			if (c2h_b_opcmp(ops[1], ops[2]) >= 0) {
				eles[0] = c2h_comb_tree(ops[1], eles[0], eles[1]);
				eles[1] = C2_NODE_PTR_INIT;
				pele = &eles[elei];
				ops[1] = ops[2];
			}
			// Otherwise, combine the second and the incoming one
			else {
				eles[1] = c2h_comb_tree(ops[2], eles[1], C2_NODE_PTR_INIT);
				assert(eles[1].type == C2_NODE_TYPE_BRANCH);
				pele = &eles[1].b->opr2;
			}
			// The last operator always needs to be reset
			ops[2] = C2_B_OUNDEFINED;
		}

		// It's a subgroup if it starts with '('
		if ('(' == pattern[offset]) {
			if ((offset = c2_parse_group(pattern, offset + 1, pele, level + 1)) < 0) {
				goto fail;
			}
		} else {
			// Otherwise it's a leaf
			if ((offset = c2_parse_target(pattern, offset, pele)) < 0) {
				goto fail;
			}

			assert(pele->type != C2_NODE_TYPE_BRANCH && pele->l != NULL);

			if ((offset = c2_parse_op(pattern, offset, pele)) < 0) {
				goto fail;
			}

			if ((offset = c2_parse_pattern(pattern, offset, pele)) < 0) {
				goto fail;
			}

			if (pele->l->predef != C2_L_PUNDEFINED) {
				typeof(pele->l->ptntype) predef_type =
				    C2_PREDEFS[pele->l->predef].is_string ? C2_L_PTSTRING
				                                          : C2_L_PTINT;
				if (C2_PREDEFS[pele->l->predef].deprecated) {
					log_warn("Predefined target \"%s\" is "
					         "deprecated. Matching against it will "
					         "always fail. Offending pattern is "
					         "\"%s\"",
					         pele->l->tgt, pattern);
				}
				if (pele->l->op == C2_L_OEXISTS) {
					pele->l->ptntype = predef_type;
				} else if (pele->l->ptntype != predef_type) {
					c2_error("Predefined target %s is a %s, but you "
					         "are comparing it with a %s",
					         C2_PREDEFS[pele->l->predef].name,
					         c2_pattern_type_names[predef_type],
					         c2_pattern_type_names[pele->l->ptntype]);
				}
			}
		}
		// Decrement offset -- we will increment it in loop update
		--offset;

		// Apply negation
		if (neg) {
			neg = false;
			pele->neg = !pele->neg;
		}

		next_expected = false;
		++elei;
		pele = &eles[elei];
	}

	// Wrong end character?
	if (pattern[offset] && !endchar) {
		c2_error("Expected end of string but found '%c'.", pattern[offset]);
	}
	if (!pattern[offset] && endchar) {
		c2_error("Expected '%c' but found end of string.", endchar);
	}

	// Handle end of group
	if (!elei) {
		c2_error("Empty group.");
	} else if (next_expected) {
		c2_error("Missing rule before end of group.");
	} else if (elei > 1) {
		assert(2 == elei);
		assert(ops[1]);
		eles[0] = c2h_comb_tree(ops[1], eles[0], eles[1]);
		eles[1] = C2_NODE_PTR_INIT;
	}

	*presult = eles[0];

	if (')' == pattern[offset]) {
		++offset;
	}

	return offset;

fail:
	c2_freep(&eles[0]);
	c2_freep(&eles[1]);

	return -1;
}

/**
 * Parse the target part of a rule.
 */
static int c2_parse_target(const char *pattern, int offset, c2_condition_node_ptr *presult) {
	// Initialize leaf
	presult->type = C2_NODE_TYPE_LEAF;
	presult->neg = false;
	presult->l = cmalloc(c2_condition_node_leaf);

	auto const pleaf = presult->l;
	*pleaf = C2_LEAF_NODE_INIT;

	// Parse negation marks
	while ('!' == pattern[offset]) {
		presult->neg = !presult->neg;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Copy target name out
	int tgtlen = 0;
	for (; pattern[offset] && (isalnum((unsigned char)pattern[offset]) ||
	                           '_' == pattern[offset] || '.' == pattern[offset]);
	     ++offset) {
		++tgtlen;
	}
	if (!tgtlen) {
		c2_error("Empty target.");
	}
	pleaf->tgt = strndup(&pattern[offset - tgtlen], (size_t)tgtlen);

	// Check for predefined targets
	static const int npredefs = (int)(sizeof(C2_PREDEFS) / sizeof(C2_PREDEFS[0]));
	for (int i = 0; i < npredefs; ++i) {
		if (strcmp(C2_PREDEFS[i].name, pleaf->tgt) == 0) {
			pleaf->predef = i;
			break;
		}
	}

	C2H_SKIP_SPACES();

	// Parse target-on-client flag
	if ('@' == pattern[offset]) {
		pleaf->target_on_client = true;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Parse index
	if ('[' == pattern[offset]) {
		if (pleaf->predef != C2_L_PUNDEFINED) {
			c2_error("Predefined targets can't have index.");
		}

		offset++;

		C2H_SKIP_SPACES();

		long index = -1;
		const char *endptr = NULL;

		if ('*' == pattern[offset]) {
			index = -1;
			endptr = pattern + offset + 1;
		} else {
			index = strtol(pattern + offset, (char **)&endptr, 0);
			if (index < 0) {
				c2_error("Index number invalid.");
			}
		}

		if (!endptr || pattern + offset == endptr) {
			c2_error("No index number found after bracket.");
		}
		if (index > INT_MAX) {
			c2_error("Index %ld too large.", index);
		}

		pleaf->index = (int)index;
		offset = to_int_checked(endptr - pattern);

		C2H_SKIP_SPACES();

		if (pattern[offset] != ']') {
			c2_error("Index end marker not found.");
		}

		++offset;

		C2H_SKIP_SPACES();
	}

	// Parse target type and format
	if (':' == pattern[offset]) {
		++offset;
		C2H_SKIP_SPACES();

		// Look for format
		bool hasformat = false;
		long format = 0;
		char *endptr = NULL;
		format = strtol(pattern + offset, &endptr, 0);
		assert(endptr);
		hasformat = endptr && endptr != pattern + offset;
		if (hasformat) {
			offset = to_int_checked(endptr - pattern);
		}
		C2H_SKIP_SPACES();

		// Look for type
		switch (pattern[offset]) {
		case 'w':
		case 'd':
		case 'c':
		case 's':
		case 'a': break;
		default: c2_error("Invalid type character.");
		}

		log_warn("Type specifier is deprecated. Type \"%c\" specified on target "
		         "\"%s\" will be ignored, you can remove it.",
		         pattern[offset], pleaf->tgt);

		offset++;
		C2H_SKIP_SPACES();

		// Write format
		if (hasformat) {
			log_warn("Format specifier is deprecated. Format \"%ld\" "
			         "specified on target \"%s\" will be ignored, you can "
			         "remove it.",
			         format, pleaf->tgt);
			if (format && format != 8 && format != 16 && format != 32) {
				c2_error("Invalid format %ld.", format);
			}
		}
	}
	return offset;

fail:
	return -1;
}

/**
 * Parse the operator part of a leaf.
 */
static int c2_parse_op(const char *pattern, int offset, c2_condition_node_ptr *presult) {
	auto const pleaf = presult->l;

	// Parse negation marks
	C2H_SKIP_SPACES();
	while ('!' == pattern[offset]) {
		presult->neg = !presult->neg;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Parse qualifiers
	if ('*' == pattern[offset] || '^' == pattern[offset] || '%' == pattern[offset] ||
	    '~' == pattern[offset]) {
		switch (pattern[offset]) {
		case '*': pleaf->match = C2_L_MCONTAINS; break;
		case '^': pleaf->match = C2_L_MSTART; break;
		case '%': pleaf->match = C2_L_MWILDCARD; break;
		case '~': pleaf->match = C2_L_MPCRE; break;
		default: assert(0);
		}
		++offset;
		C2H_SKIP_SPACES();
	}

	// Parse flags
	while ('?' == pattern[offset]) {
		pleaf->match_ignorecase = true;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Parse operator
	while ('=' == pattern[offset] || '>' == pattern[offset] || '<' == pattern[offset]) {
		if ('=' == pattern[offset] && C2_L_OGT == pleaf->op) {
			pleaf->op = C2_L_OGTEQ;
		} else if ('=' == pattern[offset] && C2_L_OLT == pleaf->op) {
			pleaf->op = C2_L_OLTEQ;
		} else if (pleaf->op) {
			c2_error("Duplicate operator.");
		} else {
			switch (pattern[offset]) {
			case '=': pleaf->op = C2_L_OEQ; break;
			case '>': pleaf->op = C2_L_OGT; break;
			case '<': pleaf->op = C2_L_OLT; break;
			default: assert(0);
			}
		}
		++offset;
		C2H_SKIP_SPACES();
	}

	// Check for problems
	if (C2_L_OEQ != pleaf->op && (pleaf->match || pleaf->match_ignorecase)) {
		c2_error("Exists/greater-than/less-than operators cannot have a "
		         "qualifier.");
	}

	return offset;

fail:
	return -1;
}

/**
 * Parse the pattern part of a leaf.
 */
static int c2_parse_pattern(const char *pattern, int offset, c2_condition_node_ptr *presult) {
	auto const pleaf = presult->l;

	// Exists operator cannot have pattern
	if (!pleaf->op) {
		return offset;
	}

	C2H_SKIP_SPACES();

	char *endptr = NULL;
	if (!strcmp_wd("true", &pattern[offset])) {
		pleaf->ptntype = C2_L_PTINT;
		pleaf->ptnint = true;
		offset += 4;        // length of "true";
	} else if (!strcmp_wd("false", &pattern[offset])) {
		pleaf->ptntype = C2_L_PTINT;
		pleaf->ptnint = false;
		offset += 5;        // length of "false";
	} else if (pleaf->ptnint = strtol(pattern + offset, &endptr, 0),
	           pattern + offset != endptr) {
		pleaf->ptntype = C2_L_PTINT;
		offset = to_int_checked(endptr - pattern);
		// Make sure we are stopping at the end of a word
		if (isalnum((unsigned char)pattern[offset])) {
			c2_error("Trailing characters after a numeric pattern.");
		}
	} else {
		// Parse string patterns
		bool raw = false;
		char delim = '\0';

		// String flags
		if (tolower((unsigned char)pattern[offset]) == 'r') {
			raw = true;
			++offset;
			C2H_SKIP_SPACES();
		}

		if (raw == true) {
			log_warn("Raw string patterns has been deprecated. pos %d", offset);
		}

		// Check for delimiters
		if (pattern[offset] == '\"' || pattern[offset] == '\'') {
			pleaf->ptntype = C2_L_PTSTRING;
			delim = pattern[offset];
			++offset;
		}

		if (pleaf->ptntype != C2_L_PTSTRING) {
			c2_error("Invalid pattern type.");
		}

		// Parse the string now
		// We can't determine the length of the pattern, so we use the length
		// to the end of the pattern string -- currently escape sequences
		// cannot be converted to a string longer than itself.
		auto tptnstr = ccalloc((strlen(pattern + offset) + 1), char);
		char *ptptnstr = tptnstr;
		pleaf->ptnstr = tptnstr;
		for (; pattern[offset] && delim != pattern[offset]; ++offset) {
			// Handle escape sequences if it's not a raw string
			if ('\\' == pattern[offset] && !raw) {
				switch (pattern[++offset]) {
				case '\\': *(ptptnstr++) = '\\'; break;
				case '\'': *(ptptnstr++) = '\''; break;
				case '\"': *(ptptnstr++) = '\"'; break;
				case 'a': *(ptptnstr++) = '\a'; break;
				case 'b': *(ptptnstr++) = '\b'; break;
				case 'f': *(ptptnstr++) = '\f'; break;
				case 'n': *(ptptnstr++) = '\n'; break;
				case 'r': *(ptptnstr++) = '\r'; break;
				case 't': *(ptptnstr++) = '\t'; break;
				case 'v': *(ptptnstr++) = '\v'; break;
				case 'o':
				case 'x': {
					scoped_charp tstr = strndup(pattern + offset + 1, 2);
					char *pstr = NULL;
					long val = strtol(
					    tstr, &pstr, ('o' == pattern[offset] ? 8 : 16));
					if (pstr != &tstr[2] || val <= 0) {
						c2_error("Invalid octal/hex escape "
						         "sequence.");
					}
					if (val > 255) {
						c2_error("Octal/hex escape sequence out "
						         "of ASCII range.");
					}
					if (val > 127) {
						// Manual sign extension
						val -= 256;
					}
					*(ptptnstr++) = (char)val;
					offset += 2;
					break;
				}
				default: c2_error("Invalid escape sequence.");
				}
			} else {
				*(ptptnstr++) = pattern[offset];
			}
		}
		if (!pattern[offset]) {
			c2_error("Premature end of pattern string.");
		}
		++offset;
		*ptptnstr = '\0';
		pleaf->ptnstr = strdup(tptnstr);
		free(tptnstr);
	}

	C2H_SKIP_SPACES();

	if (!pleaf->ptntype) {
		c2_error("Invalid pattern type.");
	}

	if (pleaf->ptntype == C2_L_PTINT) {
		if (pleaf->match) {
			c2_error("Integer/boolean pattern cannot have operator "
			         "qualifiers.");
		}
		if (pleaf->match_ignorecase) {
			c2_error("Integer/boolean pattern cannot have flags.");
		}
	}

	if (C2_L_PTSTRING == pleaf->ptntype &&
	    (C2_L_OGT == pleaf->op || C2_L_OGTEQ == pleaf->op || C2_L_OLT == pleaf->op ||
	     C2_L_OLTEQ == pleaf->op)) {
		c2_error("String pattern cannot have an arithmetic operator.");
	}

	return offset;

fail:
	return -1;
}

/**
 * Parse a condition with legacy syntax.
 */
static int c2_parse_legacy(const char *pattern, int offset, c2_condition_node_ptr *presult) {
	if (strlen(pattern + offset) < 4 || pattern[offset + 1] != ':' ||
	    !strchr(pattern + offset + 2, ':')) {
		c2_error("Legacy parser: Invalid format.");
	}

	// Allocate memory for new leaf
	auto pleaf = cmalloc(c2_condition_node_leaf);
	presult->type = C2_NODE_TYPE_LEAF;
	presult->l = pleaf;
	presult->neg = false;
	*pleaf = C2_LEAF_NODE_INIT;
	pleaf->op = C2_L_OEQ;
	pleaf->ptntype = C2_L_PTSTRING;

	// Determine the pattern target
	switch (pattern[offset]) {
	case 'n': pleaf->predef = C2_L_PNAME; break;
	case 'i': pleaf->predef = C2_L_PCLASSI; break;
	case 'g': pleaf->predef = C2_L_PCLASSG; break;
	case 'r': pleaf->predef = C2_L_PROLE; break;
	default: c2_error("Target \"%c\" invalid.\n", pattern[offset]);
	}

	offset += 2;

	// Determine the match type
	switch (pattern[offset]) {
	case 'e': pleaf->match = C2_L_MEXACT; break;
	case 'a': pleaf->match = C2_L_MCONTAINS; break;
	case 's': pleaf->match = C2_L_MSTART; break;
	case 'w': pleaf->match = C2_L_MWILDCARD; break;
	case 'p': pleaf->match = C2_L_MPCRE; break;
	default: c2_error("Type \"%c\" invalid.\n", pattern[offset]);
	}
	++offset;

	// Determine the pattern flags
	while (':' != pattern[offset]) {
		switch (pattern[offset]) {
		case 'i': pleaf->match_ignorecase = true; break;
		default: c2_error("Flag \"%c\" invalid.", pattern[offset]);
		}
		++offset;
	}
	++offset;

	// Copy the pattern
	pleaf->ptnstr = strdup(pattern + offset);

	return offset;

fail:
	return -1;
}

#undef c2_error

/**
 * Do postprocessing on a condition leaf.
 */
static bool
c2_l_postprocess(struct c2_state *state, xcb_connection_t *c, c2_condition_node_leaf *pleaf) {
	// Get target atom if it's not a predefined one
	if (pleaf->predef == C2_L_PUNDEFINED) {
		pleaf->tgtatom = get_atom_with_nul(state->atoms, pleaf->tgt, c);
		if (!pleaf->tgtatom) {
			log_error("Failed to get atom for target \"%s\".", pleaf->tgt);
			return false;
		}
	}

	// Insert target atom into tracked property name list
	if (pleaf->tgtatom) {
		struct c2_tracked_property *property;
		struct c2_tracked_property_key key = {
		    .property = pleaf->tgtatom,
		    .is_on_client = pleaf->target_on_client,
		};
		HASH_FIND(hh, state->tracked_properties, &key, sizeof(key), property);
		if (property == NULL) {
			property = cmalloc(struct c2_tracked_property);
			property->key = key;
			property->id = HASH_COUNT(state->tracked_properties);
			HASH_ADD_KEYPTR(hh, state->tracked_properties, &property->key,
			                sizeof(property->key), property);
			property->max_indices = pleaf->index;
		} else if (pleaf->index == -1) {
			property->max_indices = -1;
		} else if (property->max_indices >= 0 && pleaf->index > property->max_indices) {
			property->max_indices = pleaf->index;
		}
		pleaf->target_id = property->id;
	}

	// Warn about lower case characters in target name
	if (pleaf->predef == C2_L_PUNDEFINED) {
		for (const char *pc = pleaf->tgt; *pc; ++pc) {
			if (islower((unsigned char)*pc)) {
				log_warn("Lowercase character in target name \"%s\".",
				         pleaf->tgt);
				break;
			}
		}
	}

	// PCRE patterns
	if (C2_L_PTSTRING == pleaf->ptntype && C2_L_MPCRE == pleaf->match) {
#ifdef CONFIG_REGEX_PCRE
		int errorcode = 0;
		PCRE2_SIZE erroffset = 0;
		unsigned int options = 0;

		// Ignore case flag
		if (pleaf->match_ignorecase) {
			options |= PCRE2_CASELESS;
		}

		// Compile PCRE expression
		pleaf->regex_pcre =
		    pcre2_compile((PCRE2_SPTR)pleaf->ptnstr, PCRE2_ZERO_TERMINATED,
		                  options, &errorcode, &erroffset, NULL);
		if (pleaf->regex_pcre == NULL) {
			PCRE2_UCHAR buffer[256];
			pcre2_get_error_message(errorcode, buffer, sizeof(buffer));
			log_error("Pattern \"%s\": PCRE regular expression "
			          "parsing failed on offset %zu: %s",
			          pleaf->ptnstr, erroffset, buffer);
			return false;
		}
		pleaf->regex_pcre_match =
		    pcre2_match_data_create_from_pattern(pleaf->regex_pcre, NULL);
#else
		log_error("PCRE regular expression support not compiled in.");
		return false;
#endif
	}

	return true;
}

static bool
c2_tree_postprocess(struct c2_state *state, xcb_connection_t *c, c2_condition_node_ptr node) {
	switch (node.type) {
	case C2_NODE_TYPE_TRUE: return true;
	case C2_NODE_TYPE_LEAF: return c2_l_postprocess(state, c, node.l);
	case C2_NODE_TYPE_BRANCH:
		return c2_tree_postprocess(state, c, node.b->opr1) &&
		       c2_tree_postprocess(state, c, node.b->opr2);
	default: unreachable();
	}
}

bool c2_list_postprocess(struct c2_state *state, xcb_connection_t *c, struct list_node *list) {
	list_foreach(c2_condition, i, list, siblings) {
		if (!c2_tree_postprocess(state, c, i->root)) {
			return false;
		}
	}
	return true;
}
/**
 * Free a condition tree.
 */
static void c2_free(c2_condition_node_ptr p) {
	// For a branch element
	switch (p.type) {
	case C2_NODE_TYPE_BRANCH:
		if (p.b == NULL) {
			return;
		}

		c2_free(p.b->opr1);
		c2_free(p.b->opr2);
		free(p.b);
		return;
	case C2_NODE_TYPE_LEAF:
		// For a leaf element
		if (!p.l) {
			return;
		}

		free(p.l->tgt);
		free(p.l->ptnstr);
#ifdef CONFIG_REGEX_PCRE
		pcre2_code_free(p.l->regex_pcre);
		pcre2_match_data_free(p.l->regex_pcre_match);
#endif
		free(p.l);
	default: return;
	}
}

/**
 * Free a condition tree in c2_lptr_t.
 */
void c2_free_condition(c2_condition *lp, c2_userdata_free f) {
	if (!lp) {
		return;
	}

	if (f) {
		f(lp->data);
	}
	lp->data = NULL;
	c2_free(lp->root);
	free(lp);
}

/**
 * Get a string representation of a rule target.
 */
static const char *c2h_dump_str_tgt(const c2_condition_node_leaf *pleaf) {
	if (pleaf->predef != C2_L_PUNDEFINED) {
		return C2_PREDEFS[pleaf->predef].name;
	}
	return pleaf->tgt;
}

/**
 * Dump a condition tree to string. Return the number of characters that
 * would have been written if the buffer had been large enough, excluding
 * the null terminator.
 * null terminator will not be written to the output.
 */
static size_t
c2_condition_node_to_str(const c2_condition_node_ptr p, char *output, size_t len) {
#define push_char(c)                                                                     \
	if (offset < len)                                                                \
		output[offset] = (c);                                                    \
	offset++
#define push_str(str)                                                                    \
	do {                                                                             \
		if (offset < len) {                                                      \
			size_t slen = strlen(str);                                       \
			if (offset + slen > len)                                         \
				slen = len - offset;                                     \
			memcpy(output + offset, str, slen);                              \
		}                                                                        \
		offset += strlen(str);                                                   \
	} while (false)
	size_t offset = 0;
	char number[128];
	switch (p.type) {
	case C2_NODE_TYPE_BRANCH:
		// Branch, i.e. logical operators &&, ||, XOR
		if (p.b == NULL) {
			return 0;
		}

		if (p.neg) {
			push_char('!');
		}

		push_char('(');
		if (len > offset) {
			offset += c2_condition_node_to_str(p.b->opr1, output + offset,
			                                   len - offset);
		} else {
			offset += c2_condition_node_to_str(p.b->opr1, NULL, 0);
		}

		switch (p.b->op) {
		case C2_B_OAND: push_str(" && "); break;
		case C2_B_OOR: push_str(" || "); break;
		case C2_B_OXOR: push_str(" XOR "); break;
		default: assert(0); break;
		}

		if (len > offset) {
			offset += c2_condition_node_to_str(p.b->opr2, output + offset,
			                                   len - offset);
		} else {
			offset += c2_condition_node_to_str(p.b->opr2, NULL, 0);
		}
		push_str(")");
		break;
	case C2_NODE_TYPE_LEAF:
		// Leaf node
		if (!p.l) {
			return 0;
		}

		if (C2_L_OEXISTS == p.l->op && p.neg) {
			push_char('!');
		}

		// Print target name, type, and format
		const char *target_str = c2h_dump_str_tgt(p.l);
		push_str(target_str);
		if (p.l->target_on_client) {
			push_char('@');
		}
		if (p.l->predef == C2_L_PUNDEFINED) {
			if (p.l->index < 0) {
				push_str("[*]");
			} else {
				sprintf(number, "[%d]", p.l->index);
				push_str(number);
			}
		}

		if (C2_L_OEXISTS == p.l->op) {
			return offset;
		}

		// Print operator
		push_char(' ');

		if (C2_L_OEXISTS != p.l->op && p.neg) {
			push_char('!');
		}

		switch (p.l->match) {
		case C2_L_MEXACT: break;
		case C2_L_MCONTAINS: push_char('*'); break;
		case C2_L_MSTART: push_char('^'); break;
		case C2_L_MPCRE: push_char('~'); break;
		case C2_L_MWILDCARD: push_char('%'); break;
		}

		if (p.l->match_ignorecase) {
			push_char('?');
		}

		switch (p.l->op) {
		case C2_L_OEXISTS: break;
		case C2_L_OEQ: push_str("="); break;
		case C2_L_OGT: push_str(">"); break;
		case C2_L_OGTEQ: push_str(">="); break;
		case C2_L_OLT: push_str("<"); break;
		case C2_L_OLTEQ: push_str("<="); break;
		}

		// Print pattern
		push_char(' ');
		switch (p.l->ptntype) {
		case C2_L_PTINT:
			sprintf(number, "%ld", p.l->ptnint);
			push_str(number);
			break;
		case C2_L_PTSTRING:
			// TODO(yshui) Escape string before printing out?
			push_char('"');
			for (int i = 0; p.l->ptnstr[i]; i++) {
				switch (p.l->ptnstr[i]) {
				case '\\': push_str("\\\\"); break;
				case '"': push_str("\\\""); break;
				case '\a': push_str("\\a"); break;
				case '\b': push_str("\\b"); break;
				case '\f': push_str("\\f"); break;
				case '\r': push_str("\\r"); break;
				case '\v': push_str("\\v"); break;
				case '\t': push_str("\\t"); break;
				case '\n': push_str("\\n"); break;
				default:
					if (isprint(p.l->ptnstr[i])) {
						push_char(p.l->ptnstr[i]);
					} else {
						sprintf(number, "\\x%02x",
						        (unsigned char)p.l->ptnstr[i]);
						push_str(number);
					}
					break;
				}
			}
			push_char('"');
			break;
		default: assert(0); break;
		}
		break;
	case C2_NODE_TYPE_TRUE: push_str("(default)"); break;
	default: unreachable();
	}
#undef push_char
#undef push_str
	return offset;
}

/// Wrapper of c2_condition_to_str which uses an internal static buffer, and
/// returns a nul terminated string. The returned string is only valid until the
/// next call to this function, and should not be freed.
static const char *c2_condition_node_to_str2(c2_condition_node_ptr ptr) {
	static thread_local char buf[4096];
	auto len = c2_condition_node_to_str(ptr, buf, sizeof(buf));
	if (len >= sizeof(buf)) {
		// Resulting string is too long, clobber the last character with a nul.
		buf[sizeof(buf) - 1] = '\0';
	} else {
		buf[len] = '\0';
	}
	return buf;
}

const char *c2_condition_to_str(const c2_condition *ptr) {
	return c2_condition_node_to_str2(ptr->root);
}

/// Get the list of target number values from a struct c2_property_value
static inline const int64_t *
c2_values_get_number_targets(const struct c2_property_value *values, int index, size_t *n) {
	auto storage = values->numbers;
	if (values->length > ARR_SIZE(values->numbers)) {
		storage = values->array;
	}
	if (index < 0) {
		*n = values->length;
		return storage;
	}
	if ((size_t)index < values->length) {
		*n = 1;
		return &storage[index];
	}
	*n = 0;
	return NULL;
}

static inline bool c2_int_op(const c2_condition_node_leaf *leaf, int64_t target) {
	switch (leaf->op) {
	case C2_L_OEXISTS: return leaf->predef != C2_L_PUNDEFINED ? target : true;
	case C2_L_OEQ: return target == leaf->ptnint;
	case C2_L_OGT: return target > leaf->ptnint;
	case C2_L_OGTEQ: return target >= leaf->ptnint;
	case C2_L_OLT: return target < leaf->ptnint;
	case C2_L_OLTEQ: return target <= leaf->ptnint;
	}
	unreachable();
}

static bool c2_match_once_leaf_int(const struct win *w, const c2_condition_node_leaf *leaf) {
	// Get the value
	if (leaf->predef != C2_L_PUNDEFINED) {
		// A predefined target
		int64_t predef_target = 0;
		if (C2_PREDEFS[leaf->predef].deprecated) {
			return false;
		}

		switch (leaf->predef) {
		case C2_L_PX: predef_target = w->g.x; break;
		case C2_L_PY: predef_target = w->g.y; break;
		case C2_L_PX2: predef_target = w->g.x + w->widthb; break;
		case C2_L_PY2: predef_target = w->g.y + w->heightb; break;
		case C2_L_PWIDTH: predef_target = w->g.width; break;
		case C2_L_PHEIGHT: predef_target = w->g.height; break;
		case C2_L_PWIDTHB: predef_target = w->widthb; break;
		case C2_L_PHEIGHTB: predef_target = w->heightb; break;
		case C2_L_PBDW: predef_target = w->g.border_width; break;
		case C2_L_PFULLSCREEN: predef_target = w->is_fullscreen; break;
		case C2_L_PARGB: predef_target = win_has_alpha(w); break;
		case C2_L_PFOCUSED:
			predef_target =
			    w->a.map_state == XCB_MAP_STATE_VIEWABLE && w->is_focused;
			break;
		case C2_L_PGROUPFOCUSED:
			predef_target = w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
			                w->is_group_focused;
			break;
		case C2_L_PWMWIN: predef_target = win_is_wmwin(w); break;
		case C2_L_PBSHAPED: predef_target = w->bounding_shaped; break;
		case C2_L_PROUNDED: predef_target = w->rounded_corners; break;
		case C2_L_POVREDIR:
			// When user wants to check override-redirect, they almost always
			// want to check the client window, not the frame window. We
			// don't track the override-redirect state of the client window
			// directly, however we can assume if a window has a window
			// manager frame around it, it's not override-redirect.
			predef_target = w->a.override_redirect &&
			                wm_ref_client_of(w->tree_ref) == NULL;
			break;
		default: unreachable();
		}
		return c2_int_op(leaf, predef_target);
	}

	// A raw window property
	if (leaf->target_id == C2_L_INVALID_TARGET_ID) {
		log_debug("Leaf target ID is invalid, skipping. Most likely a list "
		          "postprocessing failure.");
		return false;
	}
	auto values = &w->c2_state.values[leaf->target_id];
	assert(!values->needs_update);
	if (!values->valid) {
		log_verbose("Property %s not found on window %#010x (%s)", leaf->tgt,
		            wm_ref_win_id(w->tree_ref), w->name);
		return false;
	}

	if (values->type == C2_PROPERTY_TYPE_STRING) {
		log_error("Property %s is not an integer", leaf->tgt);
		return false;
	}

	size_t ntargets = 0;
	auto targets = c2_values_get_number_targets(values, leaf->index, &ntargets);
	for (size_t i = 0; i < ntargets; i++) {
		if (c2_int_op(leaf, targets[i])) {
			return true;
		}
	}
	return false;
}

static bool c2_string_op(const c2_condition_node_leaf *leaf, const char *target) {
	if (leaf->op == C2_L_OEXISTS) {
		return true;
	}
	if (leaf->op != C2_L_OEQ) {
		log_error("Unsupported operator %d for string comparison.", leaf->op);
		assert(leaf->op == C2_L_OEQ);
		return false;
	}
	if (leaf->match == C2_L_MPCRE) {
#ifdef CONFIG_REGEX_PCRE
		assert(strlen(target) <= INT_MAX);
		assert(leaf->regex_pcre);
		return (pcre2_match(leaf->regex_pcre, (PCRE2_SPTR)target, strlen(target),
		                    0, 0, leaf->regex_pcre_match, NULL) > 0);
#else
		log_error("PCRE regular expression support not compiled in.");
		assert(leaf->match != C2_L_MPCRE);
		return false;
#endif
	}
	if (leaf->match_ignorecase) {
		switch (leaf->match) {
		case C2_L_MEXACT: return strcasecmp(target, leaf->ptnstr) == 0;
		case C2_L_MCONTAINS: return strcasestr(target, leaf->ptnstr);
		case C2_L_MSTART:
			return strncasecmp(target, leaf->ptnstr, strlen(leaf->ptnstr)) == 0;
		case C2_L_MWILDCARD: return !fnmatch(leaf->ptnstr, target, FNM_CASEFOLD);
		default: unreachable();
		}
	} else {
		switch (leaf->match) {
		case C2_L_MEXACT: return strcmp(target, leaf->ptnstr) == 0;
		case C2_L_MCONTAINS: return strstr(target, leaf->ptnstr);
		case C2_L_MSTART:
			return strncmp(target, leaf->ptnstr, strlen(leaf->ptnstr)) == 0;
		case C2_L_MWILDCARD: return !fnmatch(leaf->ptnstr, target, 0);
		default: unreachable();
		}
	}
	unreachable();
}

static bool c2_match_once_leaf_string(struct atom *atoms, const struct win *w,
                                      const c2_condition_node_leaf *leaf) {
	// A predefined target
	const char *predef_target = NULL;
	if (leaf->predef != C2_L_PUNDEFINED) {
		if (leaf->predef == C2_L_PWINDOWTYPE) {
			for (unsigned i = 0; i < NUM_WINTYPES; i++) {
				if (w->window_types & (1 << i) &&
				    c2_string_op(leaf, WINTYPES[i].name)) {
					return true;
				}
			}
			return false;
		}

		switch (leaf->predef) {
		case C2_L_PNAME: predef_target = w->name; break;
		case C2_L_PCLASSG: predef_target = w->class_general; break;
		case C2_L_PCLASSI: predef_target = w->class_instance; break;
		case C2_L_PROLE: predef_target = w->role; break;
		case C2_L_PWINDOWTYPE:
		default: unreachable();
		}
		if (!predef_target) {
			return false;
		}
		log_verbose("Matching against predefined target %s", predef_target);
		return c2_string_op(leaf, predef_target);
	}

	if (leaf->target_id == C2_L_INVALID_TARGET_ID) {
		log_debug("Leaf target ID is invalid, skipping. Most likely a list "
		          "postprocessing failure.");
		return false;
	}
	auto values = &w->c2_state.values[leaf->target_id];
	assert(!values->needs_update);
	if (!values->valid) {
		log_verbose("Property %s not found on window %#010x, client %#010x (%s)",
		            leaf->tgt, win_id(w), win_client_id(w, false), w->name);
		return false;
	}

	if (values->type == C2_PROPERTY_TYPE_ATOM) {
		size_t ntargets = 0;
		auto targets = c2_values_get_number_targets(values, leaf->index, &ntargets);

		for (size_t i = 0; i < ntargets; ++i) {
			auto atom = (xcb_atom_t)targets[i];
			const char *atom_name = get_atom_name_cached(atoms, atom);
			log_verbose("(%zu/%zu) Atom %u is %s", i, ntargets, atom, atom_name);
			assert(atom_name != NULL);
			if (atom_name && c2_string_op(leaf, atom_name)) {
				return true;
			}
		}
		return false;
	}

	if (values->type != C2_PROPERTY_TYPE_STRING) {
		log_verbose("Property %s is not a string", leaf->tgt);
		return false;
	}

	// Not an atom type, value is a list of nul separated strings
	if (leaf->index < 0) {
		size_t offset = 0;
		while (offset < values->length) {
			if (c2_string_op(leaf, values->string + offset)) {
				return true;
			}
			offset += strlen(values->string + offset) + 1;
		}
		return false;
	}
	size_t offset = 0;
	int index = leaf->index;
	while (offset < values->length && index != 0) {
		offset += strlen(values->string + offset) + 1;
		index -= 1;
	}
	if (index != 0 || values->length == 0) {
		// index is out of bounds
		return false;
	}
	return c2_string_op(leaf, values->string + offset);
}

/**
 * Match a window against a single leaf window condition.
 *
 * For internal use.
 */
static inline bool c2_match_once_leaf(const struct c2_state *state, const struct win *w,
                                      const c2_condition_node_ptr leaf) {
	assert(leaf.type == C2_NODE_TYPE_LEAF);
	assert(leaf.l);

	const xcb_window_t wid =
	    (leaf.l->target_on_client ? win_client_id(w, /*fallback_to_self=*/true)
	                              : win_id(w));

	// Return if wid is missing
	if (leaf.l->predef == C2_L_PUNDEFINED && !wid) {
		log_debug("Window ID missing.");
		return false;
	}

	log_verbose("Matching window %#010x (%s) against condition %s", wid, w->name,
	            c2_condition_node_to_str2(leaf));

	unsigned int pattern_type = leaf.l->ptntype;
	if (pattern_type == C2_L_PTUNDEFINED) {
		if (leaf.l->target_id == C2_L_INVALID_TARGET_ID) {
			log_debug("Leaf target ID is invalid, skipping. Most likely a "
			          "list postprocessing failure.");
			return false;
		}
		auto values = &w->c2_state.values[leaf.l->target_id];
		if (values->type == C2_PROPERTY_TYPE_STRING) {
			pattern_type = C2_L_PTSTRING;
		} else {
			pattern_type = C2_L_PTINT;
		}
	}

	switch (pattern_type) {
	// Deal with integer patterns
	case C2_L_PTINT: return c2_match_once_leaf_int(w, leaf.l);
	// String patterns
	case C2_L_PTSTRING: return c2_match_once_leaf_string(state->atoms, w, leaf.l);
	default: unreachable();
	}
}

/**
 * Match a window against a single window condition.
 *
 * @return true if matched, false otherwise.
 */
static bool c2_match_once(const struct c2_state *state, const struct win *w,
                          const c2_condition_node_ptr node) {
	bool result = false;

	switch (node.type) {
	case C2_NODE_TYPE_BRANCH:
		// Handle a branch (and/or/xor operation)
		if (!node.b) {
			return false;
		}

		log_verbose("Matching window %#010x (%s) against condition %s", win_id(w),
		            w->name, c2_condition_node_to_str2(node));

		switch (node.b->op) {
		case C2_B_OAND:
			result = (c2_match_once(state, w, node.b->opr1) &&
			          c2_match_once(state, w, node.b->opr2));
			break;
		case C2_B_OOR:
			result = (c2_match_once(state, w, node.b->opr1) ||
			          c2_match_once(state, w, node.b->opr2));
			break;
		case C2_B_OXOR:
			result = (c2_match_once(state, w, node.b->opr1) !=
			          c2_match_once(state, w, node.b->opr2));
			break;
		default: unreachable();
		}

		log_debug("(%#010x): branch: result = %d, pattern = %s", win_id(w),
		          result, c2_condition_node_to_str2(node));
		break;
	case C2_NODE_TYPE_TRUE: return true;
	case C2_NODE_TYPE_LEAF:
		// A leaf
		if (node.l == NULL) {
			return false;
		}

		result = c2_match_once_leaf(state, w, node);

		log_debug("(%#010x): leaf: result = %d, client = %#010x,  pattern = %s",
		          win_id(w), result, win_client_id(w, false),
		          c2_condition_node_to_str2(node));
		break;
	default: unreachable();
	}

	// Postprocess the result
	if (node.neg) {
		result = !result;
	}

	return result;
}

c2_condition *c2_new_true(struct list_node *list) {
	auto ret = ccalloc(1, c2_condition);
	ret->root = (c2_condition_node_ptr){.type = C2_NODE_TYPE_TRUE};
	if (list) {
		list_insert_after(list, &ret->siblings);
	}
	return ret;
}

/**
 * Match a window against a condition linked list.
 *
 * @param cache a place to cache the last matched condition
 * @param pdata a place to return the data
 * @return true if matched, false otherwise.
 */
bool c2_match(struct c2_state *state, const struct win *w,
              const struct list_node *conditions, void **pdata) {
	// Then go through the whole linked list
	list_foreach(c2_condition, i, conditions, siblings) {
		if (c2_match_once(state, w, i->root)) {
			if (pdata) {
				*pdata = i->data;
			}
			return true;
		}
	}

	return false;
}

/// Match a window against the first condition in a condition linked list.
bool c2_match_one(const struct c2_state *state, const struct win *w,
                  const c2_condition *condition, void **pdata) {
	if (!condition) {
		return false;
	}
	if (c2_match_once(state, w, condition->root)) {
		if (pdata) {
			*pdata = condition->data;
		}
		return true;
	}
	return false;
}

/// Return user data stored in a condition.
void *c2_condition_get_data(const c2_condition *condition) {
	return condition->data;
}

void *c2_condition_set_data(c2_condition *condition, void *data) {
	void *old = condition->data;
	condition->data = data;
	return old;
}

c2_condition *c2_condition_list_entry(struct list_node *list) {
	return list == NULL ? NULL : list_entry(list, c2_condition, siblings);
}

c2_condition *c2_condition_list_next(struct list_node *list, c2_condition *condition) {
	if (condition == NULL) {
		return NULL;
	}
	if (condition->siblings.next == list) {
		return NULL;
	}
	return c2_condition_list_entry(condition->siblings.next);
}

c2_condition *c2_condition_list_prev(struct list_node *list, c2_condition *condition) {
	if (condition == NULL) {
		return NULL;
	}
	if (condition->siblings.prev == list) {
		return NULL;
	}
	return c2_condition_list_entry(condition->siblings.prev);
}

struct c2_state *c2_state_new(struct atom *atoms) {
	auto ret = ccalloc(1, struct c2_state);
	ret->atoms = atoms;
	return ret;
}

void c2_state_free(struct c2_state *state) {
	struct c2_tracked_property *property, *tmp;
	HASH_ITER(hh, state->tracked_properties, property, tmp) {
		HASH_DEL(state->tracked_properties, property);
		free(property);
	}
	free(state->propert_lengths);
	free(state->cookies);
	free(state);
}

void c2_window_state_init(const struct c2_state *state, struct c2_window_state *window_state) {
	auto property_count = HASH_COUNT(state->tracked_properties);
	window_state->values = ccalloc(property_count, struct c2_property_value);
	for (size_t i = 0; i < property_count; i++) {
		window_state->values[i].needs_update = true;
		window_state->values[i].valid = false;
	}
}

void c2_window_state_destroy(const struct c2_state *state,
                             struct c2_window_state *window_state) {
	size_t property_count = HASH_COUNT(state->tracked_properties);
	for (size_t i = 0; i < property_count; i++) {
		auto values = &window_state->values[i];
		if (values->type == C2_PROPERTY_TYPE_STRING) {
			free(window_state->values[i].string);
		} else if (values->length > ARR_SIZE(values->numbers)) {
			free(window_state->values[i].array);
		}
	}
	free(window_state->values);
}

void c2_window_state_mark_dirty(const struct c2_state *state,
                                struct c2_window_state *window_state, xcb_atom_t property,
                                bool is_on_client) {
	struct c2_tracked_property *p;
	struct c2_tracked_property_key key = {
	    .property = property,
	    .is_on_client = is_on_client,
	};
	HASH_FIND(hh, state->tracked_properties, &key, sizeof(key), p);
	if (p) {
		window_state->values[p->id].needs_update = true;
	}
}

static void
c2_window_state_update_one_from_reply(struct c2_state *state,
                                      struct c2_property_value *value, xcb_atom_t property,
                                      xcb_get_property_reply_t *reply, xcb_connection_t *c) {
	auto len = to_u32_checked(xcb_get_property_value_length(reply));
	void *data = xcb_get_property_value(reply);
	bool property_is_string = x_is_type_string(state->atoms, reply->type);
	value->needs_update = false;
	value->valid = false;
	if (reply->type == XCB_ATOM_NONE) {
		// Property doesn't exist on this window
		log_verbose("Property %s doesn't exist on this window",
		            get_atom_name_cached(state->atoms, property));
		return;
	}
	if ((property_is_string && reply->format != 8) ||
	    (reply->format != 8 && reply->format != 16 && reply->format != 32)) {
		log_error("Invalid property type and format combination: property %s, "
		          "type: %s, format: %d.",
		          get_atom_name_cached(state->atoms, property),
		          get_atom_name_cached(state->atoms, reply->type), reply->format);
		return;
	}

	log_verbose("Updating property %s, length = %u, format = %d",
	            get_atom_name_cached(state->atoms, property), len, reply->format);
	value->valid = true;
	if (len == 0) {
		value->length = 0;
		return;
	}
	if (property_is_string) {
		bool nul_terminated = ((char *)data)[len - 1] == '\0';
		value->length = len;
		value->type = C2_PROPERTY_TYPE_STRING;
		if (!nul_terminated) {
			value->length += 1;
		}
		value->string = crealloc(value->string, value->length);
		memcpy(value->string, data, len);
		if (!nul_terminated) {
			value->string[len] = '\0';
		}
	} else {
		size_t step = reply->format / 8;
		bool is_signed = reply->type == XCB_ATOM_INTEGER;
		value->length = len * 8 / reply->format;
		if (reply->type == XCB_ATOM_ATOM) {
			value->type = C2_PROPERTY_TYPE_ATOM;
		} else {
			value->type = C2_PROPERTY_TYPE_NUMBER;
		}

		int64_t *storage = value->numbers;
		if (value->length > ARR_SIZE(value->numbers)) {
			if (value->capacity < value->length) {
				value->array = crealloc(value->array, value->length);
				value->capacity = value->length;
			}
			storage = value->array;
		}
		for (uint32_t i = 0; i < value->length; i++) {
			auto item = (char *)data + i * step;
			if (is_signed) {
				switch (reply->format) {
				case 8: storage[i] = *(int8_t *)item; break;
				case 16: storage[i] = *(int16_t *)item; break;
				case 32: storage[i] = *(int32_t *)item; break;
				default: unreachable();
				}
			} else {
				switch (reply->format) {
				case 8: storage[i] = *(uint8_t *)item; break;
				case 16: storage[i] = *(uint16_t *)item; break;
				case 32: storage[i] = *(uint32_t *)item; break;
				default: unreachable();
				}
			}
			log_verbose("Property %s[%d] = %" PRId64,
			            get_atom_name_cached(state->atoms, property), i,
			            storage[i]);
			if (reply->type == XCB_ATOM_ATOM) {
				// Prefetch the atom name so it will be available
				// during `c2_match`. We don't need the return value here.
				get_atom_name(state->atoms, (xcb_atom_t)storage[i], c);
			}
		}
	}
}

static void c2_window_state_update_from_replies(struct c2_state *state,
                                                struct c2_window_state *window_state,
                                                xcb_connection_t *c, xcb_window_t client_win,
                                                xcb_window_t frame_win, bool refetch) {
	HASH_ITER2(state->tracked_properties, p) {
		if (!window_state->values[p->id].needs_update) {
			continue;
		}
		xcb_window_t window = p->key.is_on_client ? client_win : frame_win;
		xcb_get_property_reply_t *reply =
		    xcb_get_property_reply(c, state->cookies[p->id], NULL);
		if (!reply) {
			log_warn("Failed to get property %d for window %#010x, some "
			         "window rules might not work.",
			         p->id, window);
			window_state->values[p->id].valid = false;
			window_state->values[p->id].needs_update = false;
			continue;
		}
		bool property_is_string = x_is_type_string(state->atoms, reply->type);
		if (reply->bytes_after > 0 && (property_is_string || p->max_indices < 0)) {
			if (!refetch) {
				log_warn("Did property %d for window %#010x change while "
				         "we were fetching it? some window rules might "
				         "not work.",
				         p->id, window);
				window_state->values[p->id].valid = false;
				window_state->values[p->id].needs_update = false;
			} else {
				state->propert_lengths[p->id] += reply->bytes_after;
				state->cookies[p->id] = xcb_get_property(
				    c, 0, window, p->key.property, XCB_GET_PROPERTY_TYPE_ANY,
				    0, (state->propert_lengths[p->id] + 3) / 4);
			}
		} else {
			c2_window_state_update_one_from_reply(
			    state, &window_state->values[p->id], p->key.property, reply, c);
		}
		free(reply);
	}
}

void c2_window_state_update(struct c2_state *state, struct c2_window_state *window_state,
                            xcb_connection_t *c, xcb_window_t client_win,
                            xcb_window_t frame_win) {
	size_t property_count = HASH_COUNT(state->tracked_properties);
	if (!state->cookies) {
		state->cookies = ccalloc(property_count, xcb_get_property_cookie_t);
	}
	if (!state->propert_lengths) {
		state->propert_lengths = ccalloc(property_count, uint32_t);
	}
	memset(state->cookies, 0, property_count * sizeof(xcb_get_property_cookie_t));

	log_verbose("Updating c2 window state for window %#010x (frame %#010x)",
	            client_win, frame_win);

	// Because we don't know the length of all properties (i.e. if they are string
	// properties, or for properties matched with `[*]`). We do this in 3 steps:
	//   1. Send requests to all properties we need. Use `max_indices` to determine
	//      the length, or use 0 if it's unknown.
	//   2. From the replies to (1), for properties we know the length of, we update
	//      the values. For those we don't, use the length information from the
	//      replies to send a new request with the correct length.
	//   3. Update the rest of the properties.

	// Step 1
	HASH_ITER2(state->tracked_properties, p) {
		if (!window_state->values[p->id].needs_update) {
			continue;
		}
		uint32_t length = 0;
		if (p->max_indices >= 0) {
			// length is in 4 bytes units
			length = (uint32_t)p->max_indices + 1;
		}

		xcb_window_t window = p->key.is_on_client ? client_win : frame_win;
		// xcb_get_property long_length is in units of 4-byte,
		// so use `ceil(length / 4)`. same below.
		state->cookies[p->id] = xcb_get_property(
		    c, 0, window, p->key.property, XCB_GET_PROPERTY_TYPE_ANY, 0, length);
		state->propert_lengths[p->id] = length * 4;
	}

	// Step 2
	c2_window_state_update_from_replies(state, window_state, c, client_win, frame_win, true);
	// Step 3
	c2_window_state_update_from_replies(state, window_state, c, client_win, frame_win,
	                                    false);
}

bool c2_state_is_property_tracked(struct c2_state *state, xcb_atom_t property) {
	struct c2_tracked_property *p;
	struct c2_tracked_property_key key = {
	    .property = property,
	    .is_on_client = true,
	};
	HASH_FIND(hh, state->tracked_properties, &key, sizeof(key), p);
	if (p != NULL) {
		return true;
	}
	key.is_on_client = false;
	HASH_FIND(hh, state->tracked_properties, &key, sizeof(key), p);
	return p != NULL;
}
