// SPDX-License-Identifier: MIT

/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

// libpcre
#ifdef CONFIG_REGEX_PCRE
#include <pcre.h>

// For compatibility with <libpcre-8.20
#ifndef PCRE_STUDY_JIT_COMPILE
#define PCRE_STUDY_JIT_COMPILE 0
#define LPCRE_FREE_STUDY(extra) pcre_free(extra)
#else
#define LPCRE_FREE_STUDY(extra) pcre_free_study(extra)
#endif

#endif

#include <X11/Xlib.h>
#include <xcb/xcb.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "string_utils.h"
#include "utils.h"
#include "win.h"
#include "x.h"
#include "atom.h"

#include "c2.h"

#pragma GCC diagnostic error "-Wunused-parameter"

#define C2_MAX_LEVELS 10

typedef struct _c2_b c2_b_t;
typedef struct _c2_l c2_l_t;

/// Pointer to a condition tree.
typedef struct {
	bool isbranch : 1;
	union {
		c2_b_t *b;
		c2_l_t *l;
	};
} c2_ptr_t;

/// Initializer for c2_ptr_t.
#define C2_PTR_INIT                                                                      \
	{ .isbranch = false, .l = NULL, }

static const c2_ptr_t C2_PTR_NULL = C2_PTR_INIT;

/// Operator of a branch element.
typedef enum {
	C2_B_OUNDEFINED,
	C2_B_OAND,
	C2_B_OOR,
	C2_B_OXOR,
} c2_b_op_t;

/// Structure for branch element in a window condition
struct _c2_b {
	bool neg : 1;
	c2_b_op_t op;
	c2_ptr_t opr1;
	c2_ptr_t opr2;
};

/// Initializer for c2_b_t.
#define C2_B_INIT                                                                        \
	{ .neg = false, .op = C2_B_OUNDEFINED, .opr1 = C2_PTR_INIT, .opr2 = C2_PTR_INIT, }

/// Structure for leaf element in a window condition
struct _c2_l {
	bool neg : 1;
	enum { C2_L_OEXISTS,
	       C2_L_OEQ,
	       C2_L_OGT,
	       C2_L_OGTEQ,
	       C2_L_OLT,
	       C2_L_OLTEQ,
	} op : 3;
	enum { C2_L_MEXACT,
	       C2_L_MSTART,
	       C2_L_MCONTAINS,
	       C2_L_MWILDCARD,
	       C2_L_MPCRE,
	} match : 3;
	bool match_ignorecase : 1;
	char *tgt;
	xcb_atom_t tgtatom;
	bool tgt_onframe;
	int index;
	enum { C2_L_PUNDEFINED = -1,
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
	enum c2_l_type {
		C2_L_TUNDEFINED,
		C2_L_TSTRING,
		C2_L_TCARDINAL,
		C2_L_TWINDOW,
		C2_L_TATOM,
		C2_L_TDRAWABLE,
	} type;
	int format;
	enum { C2_L_PTUNDEFINED,
	       C2_L_PTSTRING,
	       C2_L_PTINT,
	} ptntype;
	char *ptnstr;
	long ptnint;
#ifdef CONFIG_REGEX_PCRE
	pcre *regex_pcre;
	pcre_extra *regex_pcre_extra;
#endif
};

/// Initializer for c2_l_t.
#define C2_L_INIT                                                                           \
	{                                                                                   \
		.neg = false, .op = C2_L_OEXISTS, .match = C2_L_MEXACT,                     \
		.match_ignorecase = false, .tgt = NULL, .tgtatom = 0, .tgt_onframe = false, \
		.predef = C2_L_PUNDEFINED, .index = -1, .type = C2_L_TUNDEFINED,            \
		.format = 0, .ptntype = C2_L_PTUNDEFINED, .ptnstr = NULL, .ptnint = 0,      \
	}

static const c2_l_t leaf_def = C2_L_INIT;

/// Linked list type of conditions.
struct _c2_lptr {
	c2_ptr_t ptr;
	void *data;
	struct _c2_lptr *next;
};

/// Initializer for c2_lptr_t.
#define C2_LPTR_INIT                                                                     \
	{ .ptr = C2_PTR_INIT, .data = NULL, .next = NULL, }

/// Structure representing a predefined target.
typedef struct {
	const char *name;
	enum c2_l_type type;
	int format;
} c2_predef_t;

// Predefined targets.
static const c2_predef_t C2_PREDEFS[] = {
    [C2_L_PID] = {"id", C2_L_TCARDINAL, 0},
    [C2_L_PX] = {"x", C2_L_TCARDINAL, 0},
    [C2_L_PY] = {"y", C2_L_TCARDINAL, 0},
    [C2_L_PX2] = {"x2", C2_L_TCARDINAL, 0},
    [C2_L_PY2] = {"y2", C2_L_TCARDINAL, 0},
    [C2_L_PWIDTH] = {"width", C2_L_TCARDINAL, 0},
    [C2_L_PHEIGHT] = {"height", C2_L_TCARDINAL, 0},
    [C2_L_PWIDTHB] = {"widthb", C2_L_TCARDINAL, 0},
    [C2_L_PHEIGHTB] = {"heightb", C2_L_TCARDINAL, 0},
    [C2_L_PBDW] = {"border_width", C2_L_TCARDINAL, 0},
    [C2_L_PFULLSCREEN] = {"fullscreen", C2_L_TCARDINAL, 0},
    [C2_L_POVREDIR] = {"override_redirect", C2_L_TCARDINAL, 0},
    [C2_L_PARGB] = {"argb", C2_L_TCARDINAL, 0},
    [C2_L_PFOCUSED] = {"focused", C2_L_TCARDINAL, 0},
    [C2_L_PWMWIN] = {"wmwin", C2_L_TCARDINAL, 0},
    [C2_L_PBSHAPED] = {"bounding_shaped", C2_L_TCARDINAL, 0},
    [C2_L_PROUNDED] = {"rounded_corners", C2_L_TCARDINAL, 0},
    [C2_L_PCLIENT] = {"client", C2_L_TWINDOW, 0},
    [C2_L_PWINDOWTYPE] = {"window_type", C2_L_TSTRING, 0},
    [C2_L_PLEADER] = {"leader", C2_L_TWINDOW, 0},
    [C2_L_PNAME] = {"name", C2_L_TSTRING, 0},
    [C2_L_PCLASSG] = {"class_g", C2_L_TSTRING, 0},
    [C2_L_PCLASSI] = {"class_i", C2_L_TSTRING, 0},
    [C2_L_PROLE] = {"role", C2_L_TSTRING, 0},
};

/**
 * Get the numeric property value from a win_prop_t.
 */
static inline long winprop_get_int(winprop_t prop) {
	long tgt = 0;

	if (!prop.nitems) {
		return 0;
	}

	switch (prop.format) {
	case 8: tgt = *(prop.p8); break;
	case 16: tgt = *(prop.p16); break;
	case 32: tgt = *(prop.p32); break;
	default: assert(0); break;
	}

	return tgt;
}

/**
 * Compare next word in a string with another string.
 */
static inline int strcmp_wd(const char *needle, const char *src) {
	int ret = mstrncmp(needle, src);
	if (ret)
		return ret;

	char c = src[strlen(needle)];
	if (isalnum(c) || '_' == c)
		return 1;
	else
		return 0;
}

/**
 * Return whether a c2_ptr_t is empty.
 */
static inline bool c2_ptr_isempty(const c2_ptr_t p) {
	return !(p.isbranch ? (bool)p.b : (bool)p.l);
}

/**
 * Reset a c2_ptr_t.
 */
static inline void c2_ptr_reset(c2_ptr_t *pp) {
	if (pp)
		memcpy(pp, &C2_PTR_NULL, sizeof(c2_ptr_t));
}

/**
 * Combine two condition trees.
 */
static inline c2_ptr_t c2h_comb_tree(c2_b_op_t op, c2_ptr_t p1, c2_ptr_t p2) {
	c2_ptr_t p = {.isbranch = true, .b = cmalloc(c2_b_t)};

	p.b->opr1 = p1;
	p.b->opr2 = p2;
	p.b->op = op;

	return p;
}

/**
 * Get the precedence value of a condition branch operator.
 */
static inline int c2h_b_opp(c2_b_op_t op) {
	switch (op) {
	case C2_B_OAND: return 2;
	case C2_B_OOR: return 1;
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

static int c2_parse_grp(const char *pattern, int offset, c2_ptr_t *presult, int level);

static int c2_parse_target(const char *pattern, int offset, c2_ptr_t *presult);

static int c2_parse_op(const char *pattern, int offset, c2_ptr_t *presult);

static int c2_parse_pattern(const char *pattern, int offset, c2_ptr_t *presult);

static int c2_parse_legacy(const char *pattern, int offset, c2_ptr_t *presult);

static void c2_free(c2_ptr_t p);

/**
 * Wrapper of c2_free().
 */
static inline void c2_freep(c2_ptr_t *pp) {
	if (pp) {
		c2_free(*pp);
		c2_ptr_reset(pp);
	}
}

static const char *c2h_dump_str_tgt(const c2_l_t *pleaf);

static const char *c2h_dump_str_type(const c2_l_t *pleaf);

static void attr_unused c2_dump(c2_ptr_t p);

static xcb_atom_t c2_get_atom_type(const c2_l_t *pleaf);

static bool c2_match_once(session_t *ps, const struct managed_win *w, const c2_ptr_t cond);

/**
 * Parse a condition string.
 */
c2_lptr_t *c2_parse(c2_lptr_t **pcondlst, const char *pattern, void *data) {
	if (!pattern)
		return NULL;

	// Parse the pattern
	c2_ptr_t result = C2_PTR_INIT;
	int offset = -1;

	if (strlen(pattern) >= 2 && ':' == pattern[1])
		offset = c2_parse_legacy(pattern, 0, &result);
	else
		offset = c2_parse_grp(pattern, 0, &result, 0);

	if (offset < 0) {
		c2_freep(&result);
		return NULL;
	}

	// Insert to pcondlst
	{
		static const c2_lptr_t lptr_def = C2_LPTR_INIT;
		auto plptr = cmalloc(c2_lptr_t);
		memcpy(plptr, &lptr_def, sizeof(c2_lptr_t));
		plptr->ptr = result;
		plptr->data = data;
		if (pcondlst) {
			plptr->next = *pcondlst;
			*pcondlst = plptr;
		}

#ifdef DEBUG_C2
		log_trace("(\"%s\"): ", pattern);
		c2_dump(plptr->ptr);
#endif

		return plptr;
	}
}

#define c2_error(format, ...)                                                                \
	do {                                                                                 \
		log_error("Pattern \"%s\" pos %d: " format, pattern, offset, ##__VA_ARGS__); \
		goto fail;                                                                   \
	} while (0)

// TODO Not a very good macro
#define C2H_SKIP_SPACES()                                                                \
	{                                                                                \
		while (isspace(pattern[offset]))                                         \
			++offset;                                                        \
	}

/**
 * Parse a group in condition string.
 *
 * @return offset of next character in string
 */
static int c2_parse_grp(const char *pattern, int offset, c2_ptr_t *presult, int level) {
	// Check for recursion levels
	if (level > C2_MAX_LEVELS)
		c2_error("Exceeded maximum recursion levels.");

	if (!pattern)
		return -1;

	// Expected end character
	const char endchar = (offset ? ')' : '\0');

	// We use a system that a maximum of 2 elements are kept. When we find
	// the third element, we combine the elements according to operator
	// precedence. This design limits operators to have at most two-levels
	// of precedence and fixed left-to-right associativity.

	// For storing branch operators. ops[0] is actually unused
	c2_b_op_t ops[3] = {};
	// For storing elements
	c2_ptr_t eles[2] = {C2_PTR_INIT, C2_PTR_INIT};
	// Index of next free element slot in eles
	int elei = 0;
	// Pointer to the position of next element
	c2_ptr_t *pele = eles;
	// Negation flag of next operator
	bool neg = false;
	// Whether we are expecting an element immediately, is true at first, or
	// after encountering a logical operator
	bool next_expected = true;

	// Parse the pattern character-by-character
	for (; pattern[offset]; ++offset) {
		assert(elei <= 2);

		// Jump over spaces
		if (isspace(pattern[offset]))
			continue;

		// Handle end of group
		if (')' == pattern[offset])
			break;

		// Handle "!"
		if ('!' == pattern[offset]) {
			if (!next_expected)
				c2_error("Unexpected \"!\".");

			neg = !neg;
			continue;
		}

		// Handle AND and OR
		if ('&' == pattern[offset] || '|' == pattern[offset]) {
			if (next_expected)
				c2_error("Unexpected logical operator.");

			next_expected = true;
			if (!mstrncmp("&&", pattern + offset)) {
				ops[elei] = C2_B_OAND;
				++offset;
			} else if (!mstrncmp("||", pattern + offset)) {
				ops[elei] = C2_B_OOR;
				++offset;
			} else
				c2_error("Illegal logical operator.");

			continue;
		}

		// Parsing an element
		if (!next_expected)
			c2_error("Unexpected expression.");

		assert(!elei || ops[elei]);

		// If we are out of space
		if (2 == elei) {
			--elei;
			// If the first operator has higher or equal precedence, combine
			// the first two elements
			if (c2h_b_opcmp(ops[1], ops[2]) >= 0) {
				eles[0] = c2h_comb_tree(ops[1], eles[0], eles[1]);
				c2_ptr_reset(&eles[1]);
				pele = &eles[elei];
				ops[1] = ops[2];
			}
			// Otherwise, combine the second and the incoming one
			else {
				eles[1] = c2h_comb_tree(ops[2], eles[1], C2_PTR_NULL);
				assert(eles[1].isbranch);
				pele = &eles[1].b->opr2;
			}
			// The last operator always needs to be reset
			ops[2] = C2_B_OUNDEFINED;
		}

		// It's a subgroup if it starts with '('
		if ('(' == pattern[offset]) {
			if ((offset = c2_parse_grp(pattern, offset + 1, pele, level + 1)) < 0)
				goto fail;
		}
		// Otherwise it's a leaf
		else {
			if ((offset = c2_parse_target(pattern, offset, pele)) < 0)
				goto fail;

			assert(!pele->isbranch && !c2_ptr_isempty(*pele));

			if ((offset = c2_parse_op(pattern, offset, pele)) < 0)
				goto fail;

			if ((offset = c2_parse_pattern(pattern, offset, pele)) < 0)
				goto fail;
		}
		// Decrement offset -- we will increment it in loop update
		--offset;

		// Apply negation
		if (neg) {
			neg = false;
			if (pele->isbranch)
				pele->b->neg = !pele->b->neg;
			else
				pele->l->neg = !pele->l->neg;
		}

		next_expected = false;
		++elei;
		pele = &eles[elei];
	}

	// Wrong end character?
	if (pattern[offset] && !endchar)
		c2_error("Expected end of string but found '%c'.", pattern[offset]);
	if (!pattern[offset] && endchar)
		c2_error("Expected '%c' but found end of string.", endchar);

	// Handle end of group
	if (!elei) {
		c2_error("Empty group.");
	} else if (next_expected) {
		c2_error("Missing rule before end of group.");
	} else if (elei > 1) {
		assert(2 == elei);
		assert(ops[1]);
		eles[0] = c2h_comb_tree(ops[1], eles[0], eles[1]);
		c2_ptr_reset(&eles[1]);
	}

	*presult = eles[0];

	if (')' == pattern[offset])
		++offset;

	return offset;

fail:
	c2_freep(&eles[0]);
	c2_freep(&eles[1]);

	return -1;
}

/**
 * Parse the target part of a rule.
 */
static int c2_parse_target(const char *pattern, int offset, c2_ptr_t *presult) {
	// Initialize leaf
	presult->isbranch = false;
	presult->l = cmalloc(c2_l_t);

	c2_l_t *const pleaf = presult->l;
	memcpy(pleaf, &leaf_def, sizeof(c2_l_t));

	// Parse negation marks
	while ('!' == pattern[offset]) {
		pleaf->neg = !pleaf->neg;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Copy target name out
	int tgtlen = 0;
	for (; pattern[offset] && (isalnum(pattern[offset]) || '_' == pattern[offset]);
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
		if (!strcmp(C2_PREDEFS[i].name, pleaf->tgt)) {
			pleaf->predef = i;
			pleaf->type = C2_PREDEFS[i].type;
			pleaf->format = C2_PREDEFS[i].format;
			break;
		}
	}

	C2H_SKIP_SPACES();

	// Parse target-on-frame flag
	if ('@' == pattern[offset]) {
		pleaf->tgt_onframe = true;
		++offset;
		C2H_SKIP_SPACES();
	}

	// Parse index
	if ('[' == pattern[offset]) {
		offset++;

		C2H_SKIP_SPACES();

		long index = -1;
		char *endptr = NULL;

		index = strtol(pattern + offset, &endptr, 0);

		if (!endptr || pattern + offset == endptr) {
			c2_error("No index number found after bracket.");
		}
		if (index < 0) {
			c2_error("Index number invalid.");
		}
		if (pleaf->predef != C2_L_PUNDEFINED) {
			c2_error("Predefined targets can't have index.");
		}

		pleaf->index = to_int_checked(index);
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
		{
			char *endptr = NULL;
			format = strtol(pattern + offset, &endptr, 0);
			assert(endptr);
			if ((hasformat = (endptr && endptr != pattern + offset))) {
				offset = to_int_checked(endptr - pattern);
			}
			C2H_SKIP_SPACES();
		}

		// Look for type
		enum c2_l_type type = C2_L_TUNDEFINED;
		switch (pattern[offset]) {
		case 'w': type = C2_L_TWINDOW; break;
		case 'd': type = C2_L_TDRAWABLE; break;
		case 'c': type = C2_L_TCARDINAL; break;
		case 's': type = C2_L_TSTRING; break;
		case 'a': type = C2_L_TATOM; break;
		default: c2_error("Invalid type character.");
		}

		if (type) {
			if (pleaf->predef != C2_L_PUNDEFINED) {
				log_warn("Type specified for a default target "
				         "will be ignored.");
			} else {
				if (pleaf->type && type != pleaf->type)
					log_warn("Default type overridden on "
					         "target.");
				pleaf->type = type;
			}
		}

		offset++;
		C2H_SKIP_SPACES();

		// Default format
		if (!pleaf->format) {
			switch (pleaf->type) {
			case C2_L_TWINDOW:
			case C2_L_TDRAWABLE:
			case C2_L_TATOM: pleaf->format = 32; break;
			case C2_L_TSTRING: pleaf->format = 8; break;
			default: break;
			}
		}

		// Write format
		if (hasformat) {
			if (pleaf->predef != C2_L_PUNDEFINED) {
				log_warn("Format \"%ld\" specified on a default target "
				         "will be ignored.",
				         format);
			} else if (pleaf->type == C2_L_TSTRING) {
				log_warn("Format \"%ld\" specified on a string target "
				         "will be ignored.",
				         format);
			} else {
				if (pleaf->format && pleaf->format != format)
					log_warn("Default format %d overridden on "
					         "target.",
					         pleaf->format);
				pleaf->format = to_int_checked(format);
			}
		}
	}

	if (!pleaf->type)
		c2_error("Target type cannot be determined.");

	// if (!pleaf->predef && !pleaf->format && C2_L_TSTRING != pleaf->type)
	//   c2_error("Target format cannot be determined.");

	if (pleaf->format && 8 != pleaf->format && 16 != pleaf->format && 32 != pleaf->format)
		c2_error("Invalid format.");

	return offset;

fail:
	return -1;
}

/**
 * Parse the operator part of a leaf.
 */
static int c2_parse_op(const char *pattern, int offset, c2_ptr_t *presult) {
	c2_l_t *const pleaf = presult->l;

	// Parse negation marks
	C2H_SKIP_SPACES();
	while ('!' == pattern[offset]) {
		pleaf->neg = !pleaf->neg;
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
		if ('=' == pattern[offset] && C2_L_OGT == pleaf->op)
			pleaf->op = C2_L_OGTEQ;
		else if ('=' == pattern[offset] && C2_L_OLT == pleaf->op)
			pleaf->op = C2_L_OLTEQ;
		else if (pleaf->op) {
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
	if (C2_L_OEQ != pleaf->op && (pleaf->match || pleaf->match_ignorecase))
		c2_error("Exists/greater-than/less-than operators cannot have a "
		         "qualifier.");

	return offset;

fail:
	return -1;
}

/**
 * Parse the pattern part of a leaf.
 */
static int c2_parse_pattern(const char *pattern, int offset, c2_ptr_t *presult) {
	c2_l_t *const pleaf = presult->l;

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
		if (isalnum(pattern[offset])) {
			c2_error("Trailing characters after a numeric pattern.");
		}
	} else {
		// Parse string patterns
		bool raw = false;
		char delim = '\0';

		// String flags
		if (tolower(pattern[offset]) == 'r') {
			raw = true;
			++offset;
			C2H_SKIP_SPACES();
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
					char *tstr = strndup(pattern + offset + 1, 2);
					char *pstr = NULL;
					long val = strtol(
					    tstr, &pstr, ('o' == pattern[offset] ? 8 : 16));
					free(tstr);
					if (pstr != &tstr[2] || val <= 0)
						c2_error("Invalid octal/hex escape "
						         "sequence.");
					*(ptptnstr++) = to_char_checked(val);
					offset += 2;
					break;
				}
				default: c2_error("Invalid escape sequence.");
				}
			} else {
				*(ptptnstr++) = pattern[offset];
			}
		}
		if (!pattern[offset])
			c2_error("Premature end of pattern string.");
		++offset;
		*ptptnstr = '\0';
		pleaf->ptnstr = strdup(tptnstr);
		free(tptnstr);
	}

	C2H_SKIP_SPACES();

	if (!pleaf->ptntype)
		c2_error("Invalid pattern type.");

	// Check if the type is correct
	if (!(((C2_L_TSTRING == pleaf->type || C2_L_TATOM == pleaf->type) &&
	       C2_L_PTSTRING == pleaf->ptntype) ||
	      ((C2_L_TCARDINAL == pleaf->type || C2_L_TWINDOW == pleaf->type ||
	        C2_L_TDRAWABLE == pleaf->type) &&
	       C2_L_PTINT == pleaf->ptntype)))
		c2_error("Pattern type incompatible with target type.");

	if (C2_L_PTINT == pleaf->ptntype && pleaf->match)
		c2_error("Integer/boolean pattern cannot have operator qualifiers.");

	if (C2_L_PTINT == pleaf->ptntype && pleaf->match_ignorecase)
		c2_error("Integer/boolean pattern cannot have flags.");

	if (C2_L_PTSTRING == pleaf->ptntype &&
	    (C2_L_OGT == pleaf->op || C2_L_OGTEQ == pleaf->op || C2_L_OLT == pleaf->op ||
	     C2_L_OLTEQ == pleaf->op))
		c2_error("String pattern cannot have an arithmetic operator.");

	return offset;

fail:
	return -1;
}

/**
 * Parse a condition with legacy syntax.
 */
static int c2_parse_legacy(const char *pattern, int offset, c2_ptr_t *presult) {
	if (strlen(pattern + offset) < 4 || pattern[offset + 1] != ':' ||
	    !strchr(pattern + offset + 2, ':')) {
		c2_error("Legacy parser: Invalid format.");
	}

	// Allocate memory for new leaf
	auto pleaf = cmalloc(c2_l_t);
	presult->isbranch = false;
	presult->l = pleaf;
	memcpy(pleaf, &leaf_def, sizeof(c2_l_t));
	pleaf->type = C2_L_TSTRING;
	pleaf->op = C2_L_OEQ;
	pleaf->ptntype = C2_L_PTSTRING;

	// Determine the pattern target
#define TGTFILL(pdefid)                                                                  \
	(pleaf->predef = pdefid, pleaf->type = C2_PREDEFS[pdefid].type,                  \
	 pleaf->format = C2_PREDEFS[pdefid].format)
	switch (pattern[offset]) {
	case 'n': TGTFILL(C2_L_PNAME); break;
	case 'i': TGTFILL(C2_L_PCLASSI); break;
	case 'g': TGTFILL(C2_L_PCLASSG); break;
	case 'r': TGTFILL(C2_L_PROLE); break;
	default: c2_error("Target \"%c\" invalid.\n", pattern[offset]);
	}
#undef TGTFILL

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
static bool c2_l_postprocess(session_t *ps, c2_l_t *pleaf) {
	// Give a pattern type to a leaf with exists operator, if needed
	if (C2_L_OEXISTS == pleaf->op && !pleaf->ptntype) {
		pleaf->ptntype = (C2_L_TSTRING == pleaf->type ? C2_L_PTSTRING : C2_L_PTINT);
	}

	// Get target atom if it's not a predefined one
	if (pleaf->predef == C2_L_PUNDEFINED) {
		pleaf->tgtatom = get_atom(ps->atoms, pleaf->tgt);
		if (!pleaf->tgtatom) {
			log_error("Failed to get atom for target \"%s\".", pleaf->tgt);
			return false;
		}
	}

	// Insert target Atom into atom track list
	if (pleaf->tgtatom) {
		bool found = false;
		for (latom_t *platom = ps->track_atom_lst; platom; platom = platom->next) {
			if (pleaf->tgtatom == platom->atom) {
				found = true;
				break;
			}
		}
		if (!found) {
			auto pnew = cmalloc(latom_t);
			pnew->next = ps->track_atom_lst;
			pnew->atom = pleaf->tgtatom;
			ps->track_atom_lst = pnew;
		}
	}

	// Enable specific tracking options in compton if needed by the condition
	// TODO: Add track_leader
	switch (pleaf->predef) {
	// case C2_L_PROUNDED: ps->o.detect_rounded_corners = true; break;
	case C2_L_PNAME:
	case C2_L_PCLASSG:
	case C2_L_PCLASSI:
	case C2_L_PROLE: ps->o.track_wdata = true; break;
	default: break;
	}

	// Warn about lower case characters in target name
	if (pleaf->predef == C2_L_PUNDEFINED) {
		for (const char *pc = pleaf->tgt; *pc; ++pc) {
			if (islower(*pc)) {
				log_warn("Lowercase character in target name \"%s\".",
				         pleaf->tgt);
				break;
			}
		}
	}

	// PCRE patterns
	if (C2_L_PTSTRING == pleaf->ptntype && C2_L_MPCRE == pleaf->match) {
#ifdef CONFIG_REGEX_PCRE
		const char *error = NULL;
		int erroffset = 0;
		int options = 0;

		// Ignore case flag
		if (pleaf->match_ignorecase)
			options |= PCRE_CASELESS;

		// Compile PCRE expression
		pleaf->regex_pcre =
		    pcre_compile(pleaf->ptnstr, options, &error, &erroffset, NULL);
		if (!pleaf->regex_pcre) {
			log_error("Pattern \"%s\": PCRE regular expression parsing "
			          "failed on "
			          "offset %d: %s",
			          pleaf->ptnstr, erroffset, error);
			return false;
		}
#ifdef CONFIG_REGEX_PCRE_JIT
		pleaf->regex_pcre_extra =
		    pcre_study(pleaf->regex_pcre, PCRE_STUDY_JIT_COMPILE, &error);
		if (!pleaf->regex_pcre_extra) {
			printf("Pattern \"%s\": PCRE regular expression study failed: %s",
			       pleaf->ptnstr, error);
		}
#endif

		// Free the target string
		// free(pleaf->tgt);
		// pleaf->tgt = NULL;
#else
		log_error("PCRE regular expression support not compiled in.");
		return false;
#endif
	}

	return true;
}

static bool c2_tree_postprocess(session_t *ps, c2_ptr_t node) {
	if (!node.isbranch) {
		return c2_l_postprocess(ps, node.l);
	}
	if (!c2_tree_postprocess(ps, node.b->opr1))
		return false;
	return c2_tree_postprocess(ps, node.b->opr2);
}

bool c2_list_postprocess(session_t *ps, c2_lptr_t *list) {
	c2_lptr_t *head = list;
	while (head) {
		if (!c2_tree_postprocess(ps, head->ptr))
			return false;
		head = head->next;
	}
	return true;
}
/**
 * Free a condition tree.
 */
static void c2_free(c2_ptr_t p) {
	// For a branch element
	if (p.isbranch) {
		c2_b_t *const pbranch = p.b;

		if (!pbranch)
			return;

		c2_free(pbranch->opr1);
		c2_free(pbranch->opr2);
		free(pbranch);
	}
	// For a leaf element
	else {
		c2_l_t *const pleaf = p.l;

		if (!pleaf)
			return;

		free(pleaf->tgt);
		free(pleaf->ptnstr);
#ifdef CONFIG_REGEX_PCRE
		pcre_free(pleaf->regex_pcre);
		LPCRE_FREE_STUDY(pleaf->regex_pcre_extra);
#endif
		free(pleaf);
	}
}

/**
 * Free a condition tree in c2_lptr_t.
 */
c2_lptr_t *c2_free_lptr(c2_lptr_t *lp) {
	if (!lp)
		return NULL;

	c2_lptr_t *pnext = lp->next;
	c2_free(lp->ptr);
	free(lp);

	return pnext;
}

/**
 * Get a string representation of a rule target.
 */
static const char *c2h_dump_str_tgt(const c2_l_t *pleaf) {
	if (pleaf->predef != C2_L_PUNDEFINED) {
		return C2_PREDEFS[pleaf->predef].name;
	} else {
		return pleaf->tgt;
	}
}

/**
 * Get a string representation of a target.
 */
static const char *c2h_dump_str_type(const c2_l_t *pleaf) {
	switch (pleaf->type) {
	case C2_L_TWINDOW: return "w";
	case C2_L_TDRAWABLE: return "d";
	case C2_L_TCARDINAL: return "c";
	case C2_L_TSTRING: return "s";
	case C2_L_TATOM: return "a";
	case C2_L_TUNDEFINED: break;
	}

	return NULL;
}

/**
 * Dump a condition tree.
 */
static void c2_dump(c2_ptr_t p) {
	// For a branch
	if (p.isbranch) {
		const c2_b_t *const pbranch = p.b;

		if (!pbranch)
			return;

		if (pbranch->neg)
			putchar('!');

		printf("(");
		c2_dump(pbranch->opr1);

		switch (pbranch->op) {
		case C2_B_OAND: printf(" && "); break;
		case C2_B_OOR: printf(" || "); break;
		case C2_B_OXOR: printf(" XOR "); break;
		default: assert(0); break;
		}

		c2_dump(pbranch->opr2);
		printf(")");
	}
	// For a leaf
	else {
		const c2_l_t *const pleaf = p.l;

		if (!pleaf)
			return;

		if (C2_L_OEXISTS == pleaf->op && pleaf->neg)
			putchar('!');

		// Print target name, type, and format
		{
			printf("%s", c2h_dump_str_tgt(pleaf));
			if (pleaf->tgt_onframe)
				putchar('@');
			if (pleaf->index >= 0)
				printf("[%d]", pleaf->index);
			printf(":%d%s", pleaf->format, c2h_dump_str_type(pleaf));
		}

		// Print operator
		putchar(' ');

		if (C2_L_OEXISTS != pleaf->op && pleaf->neg)
			putchar('!');

		switch (pleaf->match) {
		case C2_L_MEXACT: break;
		case C2_L_MCONTAINS: putchar('*'); break;
		case C2_L_MSTART: putchar('^'); break;
		case C2_L_MPCRE: putchar('~'); break;
		case C2_L_MWILDCARD: putchar('%'); break;
		}

		if (pleaf->match_ignorecase)
			putchar('?');

		switch (pleaf->op) {
		case C2_L_OEXISTS: break;
		case C2_L_OEQ: fputs("=", stdout); break;
		case C2_L_OGT: fputs(">", stdout); break;
		case C2_L_OGTEQ: fputs(">=", stdout); break;
		case C2_L_OLT: fputs("<", stdout); break;
		case C2_L_OLTEQ: fputs("<=", stdout); break;
		}

		if (C2_L_OEXISTS == pleaf->op)
			return;

		// Print pattern
		putchar(' ');
		switch (pleaf->ptntype) {
		case C2_L_PTINT: printf("%ld", pleaf->ptnint); break;
		case C2_L_PTSTRING:
			// TODO: Escape string before printing out?
			printf("\"%s\"", pleaf->ptnstr);
			break;
		default: assert(0); break;
		}
	}
}

/**
 * Get the type atom of a condition.
 */
static xcb_atom_t c2_get_atom_type(const c2_l_t *pleaf) {
	switch (pleaf->type) {
	case C2_L_TCARDINAL: return XCB_ATOM_CARDINAL;
	case C2_L_TWINDOW: return XCB_ATOM_WINDOW;
	case C2_L_TSTRING: return XCB_ATOM_STRING;
	case C2_L_TATOM: return XCB_ATOM_ATOM;
	case C2_L_TDRAWABLE: return XCB_ATOM_DRAWABLE;
	default: assert(0); break;
	}
	unreachable;
}

/**
 * Match a window against a single leaf window condition.
 *
 * For internal use.
 */
static inline void c2_match_once_leaf(session_t *ps, const struct managed_win *w,
                                      const c2_l_t *pleaf, bool *pres, bool *perr) {
	assert(pleaf);

	const xcb_window_t wid = (pleaf->tgt_onframe ? w->client_win : w->base.id);

	// Return if wid is missing
	if (pleaf->predef == C2_L_PUNDEFINED && !wid) {
		return;
	}

	const int idx = (pleaf->index < 0 ? 0 : pleaf->index);

	switch (pleaf->ptntype) {
	// Deal with integer patterns
	case C2_L_PTINT: {
		long tgt = 0;

		// Get the value
		// A predefined target
		if (pleaf->predef != C2_L_PUNDEFINED) {
			*perr = false;
			switch (pleaf->predef) {
			case C2_L_PID: tgt = wid; break;
			case C2_L_PX: tgt = w->g.x; break;
			case C2_L_PY: tgt = w->g.y; break;
			case C2_L_PX2: tgt = w->g.x + w->widthb; break;
			case C2_L_PY2: tgt = w->g.y + w->heightb; break;
			case C2_L_PWIDTH: tgt = w->g.width; break;
			case C2_L_PHEIGHT: tgt = w->g.height; break;
			case C2_L_PWIDTHB: tgt = w->widthb; break;
			case C2_L_PHEIGHTB: tgt = w->heightb; break;
			case C2_L_PBDW: tgt = w->g.border_width; break;
			case C2_L_PFULLSCREEN: tgt = win_is_fullscreen(ps, w); break;
			case C2_L_POVREDIR: tgt = w->a.override_redirect; break;
			case C2_L_PARGB: tgt = win_has_alpha(w); break;
			case C2_L_PFOCUSED: tgt = win_is_focused_real(ps, w); break;
			case C2_L_PWMWIN: tgt = w->wmwin; break;
			case C2_L_PBSHAPED: tgt = w->bounding_shaped; break;
			case C2_L_PROUNDED: tgt = w->rounded_corners; break;
			case C2_L_PCLIENT: tgt = w->client_win; break;
			case C2_L_PLEADER: tgt = w->leader; break;
			default:
				*perr = true;
				assert(0);
				break;
			}
		}
		// A raw window property
		else {
			winprop_t prop =
			    wid_get_prop_adv(ps, wid, pleaf->tgtatom, idx, 1L,
			                     c2_get_atom_type(pleaf), pleaf->format);
			if (prop.nitems) {
				*perr = false;
				tgt = winprop_get_int(prop);
			}
			free_winprop(&prop);
		}

		if (*perr)
			return;

		// Do comparison
		switch (pleaf->op) {
		case C2_L_OEXISTS:
			*pres = (pleaf->predef != C2_L_PUNDEFINED ? tgt : true);
			break;
		case C2_L_OEQ: *pres = (tgt == pleaf->ptnint); break;
		case C2_L_OGT: *pres = (tgt > pleaf->ptnint); break;
		case C2_L_OGTEQ: *pres = (tgt >= pleaf->ptnint); break;
		case C2_L_OLT: *pres = (tgt < pleaf->ptnint); break;
		case C2_L_OLTEQ: *pres = (tgt <= pleaf->ptnint); break;
		default:
			*perr = true;
			assert(0);
			break;
		}
	} break;
	// String patterns
	case C2_L_PTSTRING: {
		const char *tgt = NULL;
		char *tgt_free = NULL;

		// A predefined target
		if (pleaf->predef != C2_L_PUNDEFINED) {
			switch (pleaf->predef) {
			case C2_L_PWINDOWTYPE: tgt = WINTYPES[w->window_type]; break;
			case C2_L_PNAME: tgt = w->name; break;
			case C2_L_PCLASSG: tgt = w->class_general; break;
			case C2_L_PCLASSI: tgt = w->class_instance; break;
			case C2_L_PROLE: tgt = w->role; break;
			default: assert(0); break;
			}
		} else if (pleaf->type == C2_L_TATOM) {
			// An atom type property, convert it to string
			winprop_t prop =
			    wid_get_prop_adv(ps, wid, pleaf->tgtatom, idx, 1L,
			                     c2_get_atom_type(pleaf), pleaf->format);
			xcb_atom_t atom = (xcb_atom_t)winprop_get_int(prop);
			if (atom) {
				xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(
				    ps->c, xcb_get_atom_name(ps->c, atom), NULL);
				if (reply) {
					tgt_free = strndup(
					    xcb_get_atom_name_name(reply),
					    (size_t)xcb_get_atom_name_name_length(reply));
					free(reply);
				}
			}
			if (tgt_free) {
				tgt = tgt_free;
			}
			free_winprop(&prop);
		} else {
			// Not an atom type, just fetch the string list
			char **strlst = NULL;
			int nstr;
			if (wid_get_text_prop(ps, wid, pleaf->tgtatom, &strlst, &nstr) &&
			    nstr > idx) {
				tgt_free = strdup(strlst[idx]);
				tgt = tgt_free;
			}
			if (strlst)
				XFreeStringList(strlst);
		}

		if (tgt) {
			*perr = false;
		} else {
			return;
		}

		// Actual matching
		switch (pleaf->op) {
		case C2_L_OEXISTS: *pres = true; break;
		case C2_L_OEQ:
			switch (pleaf->match) {
			case C2_L_MEXACT:
				if (pleaf->match_ignorecase)
					*pres = !strcasecmp(tgt, pleaf->ptnstr);
				else
					*pres = !strcmp(tgt, pleaf->ptnstr);
				break;
			case C2_L_MCONTAINS:
				if (pleaf->match_ignorecase)
					*pres = strcasestr(tgt, pleaf->ptnstr);
				else
					*pres = strstr(tgt, pleaf->ptnstr);
				break;
			case C2_L_MSTART:
				if (pleaf->match_ignorecase)
					*pres = !strncasecmp(tgt, pleaf->ptnstr,
					                     strlen(pleaf->ptnstr));
				else
					*pres = !strncmp(tgt, pleaf->ptnstr,
					                 strlen(pleaf->ptnstr));
				break;
			case C2_L_MWILDCARD: {
				int flags = 0;
				if (pleaf->match_ignorecase)
					flags |= FNM_CASEFOLD;
				*pres = !fnmatch(pleaf->ptnstr, tgt, flags);
			} break;
			case C2_L_MPCRE:
#ifdef CONFIG_REGEX_PCRE
				assert(strlen(tgt) <= INT_MAX);
				*pres =
				    (pcre_exec(pleaf->regex_pcre, pleaf->regex_pcre_extra,
				               tgt, (int)strlen(tgt), 0, 0, NULL, 0) >= 0);
#else
				assert(0);
#endif
				break;
			}
			break;
		default: *perr = true; assert(0);
		}

		// Free the string after usage, if necessary
		if (tgt_free) {
			if (C2_L_TATOM == pleaf->type)
				XFree(tgt_free);
			else
				free(tgt_free);
		}
	} break;
	default: assert(0); break;
	}
}

/**
 * Match a window against a single window condition.
 *
 * @return true if matched, false otherwise.
 */
static bool c2_match_once(session_t *ps, const struct managed_win *w, const c2_ptr_t cond) {
	bool result = false;
	bool error = true;

	// Handle a branch
	if (cond.isbranch) {
		const c2_b_t *pb = cond.b;

		if (!pb)
			return false;

		error = false;

		switch (pb->op) {
		case C2_B_OAND:
			result = (c2_match_once(ps, w, pb->opr1) &&
			          c2_match_once(ps, w, pb->opr2));
			break;
		case C2_B_OOR:
			result = (c2_match_once(ps, w, pb->opr1) ||
			          c2_match_once(ps, w, pb->opr2));
			break;
		case C2_B_OXOR:
			result = (c2_match_once(ps, w, pb->opr1) !=
			          c2_match_once(ps, w, pb->opr2));
			break;
		default: error = true; assert(0);
		}

#ifdef DEBUG_WINMATCH
		log_trace("(%#010lx): branch: result = %d, pattern = ", w->id, result);
		c2_dump(cond);
#endif
	}
	// Handle a leaf
	else {
		const c2_l_t *pleaf = cond.l;

		if (!pleaf)
			return false;

		c2_match_once_leaf(ps, w, pleaf, &result, &error);

		// For EXISTS operator, no errors are fatal
		if (C2_L_OEXISTS == pleaf->op && error) {
			result = false;
			error = false;
		}

#ifdef DEBUG_WINMATCH
		log_trace("(%#010lx): leaf: result = %d, error = %d, "
		          "client = %#010lx,  pattern = ",
		          w->id, result, error, w->client_win);
		c2_dump(cond);
#endif
	}

	// Postprocess the result
	if (error)
		result = false;

	if (cond.isbranch ? cond.b->neg : cond.l->neg)
		result = !result;

	return result;
}

/**
 * Match a window against a condition linked list.
 *
 * @param cache a place to cache the last matched condition
 * @param pdata a place to return the data
 * @return true if matched, false otherwise.
 */
bool c2_match(session_t *ps, const struct managed_win *w, const c2_lptr_t *condlst, void **pdata) {
	// Then go through the whole linked list
	for (; condlst; condlst = condlst->next) {
		if (c2_match_once(ps, w, condlst->ptr)) {
			if (pdata)
				*pdata = condlst->data;
			return true;
		}
	}

	return false;
}
