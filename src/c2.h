/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "common.h"

#include <fnmatch.h>
#include <ctype.h>

// libpcre
#ifdef CONFIG_REGEX_PCRE
#include <pcre.h>

// For compatiblity with <libpcre-8.20
#ifndef PCRE_STUDY_JIT_COMPILE
#define PCRE_STUDY_JIT_COMPILE    0
#define LPCRE_FREE_STUDY(extra)   pcre_free(extra)
#else
#define LPCRE_FREE_STUDY(extra)   pcre_free_study(extra)
#endif

#endif

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
#define C2_PTR_INIT { \
  .isbranch = false, \
  .l = NULL, \
}

const static c2_ptr_t C2_PTR_NULL = C2_PTR_INIT;

/// Operator of a branch element.
typedef enum {
  C2_B_OUNDEFINED,
  C2_B_OAND,
  C2_B_OOR,
  C2_B_OXOR,
} c2_b_op_t;

/// Structure for branch element in a window condition
struct _c2_b {
  bool neg      : 1;
  c2_b_op_t op;
  c2_ptr_t opr1;
  c2_ptr_t opr2;
};

/// Initializer for c2_b_t.
#define C2_B_INIT { \
  .neg = false, \
  .op = C2_B_OUNDEFINED, \
  .opr1 = C2_PTR_INIT, \
  .opr2 = C2_PTR_INIT, \
}

/// Structure for leaf element in a window condition
struct _c2_l {
  bool neg    : 1;
  enum {
    C2_L_OEXISTS,
    C2_L_OEQ,
    C2_L_OGT,
    C2_L_OGTEQ,
    C2_L_OLT,
    C2_L_OLTEQ,
  } op        : 3;
  enum {
    C2_L_MEXACT,
    C2_L_MSTART,
    C2_L_MCONTAINS,
    C2_L_MWILDCARD,
    C2_L_MPCRE,
  } match     : 3;
  bool match_ignorecase : 1;
  char *tgt;
  Atom tgtatom;
  bool tgt_onframe;
  int index;
  enum {
    C2_L_PUNDEFINED,
    C2_L_PID,
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
  enum {
    C2_L_PTUNDEFINED,
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
#define C2_L_INIT { \
  .neg = false, \
  .op = C2_L_OEXISTS, \
  .match = C2_L_MEXACT, \
  .match_ignorecase = false, \
  .tgt = NULL, \
  .tgtatom = 0, \
  .tgt_onframe = false, \
  .predef = C2_L_PUNDEFINED, \
  .index = -1, \
  .type = C2_L_TUNDEFINED, \
  .format = 0, \
  .ptntype = C2_L_PTUNDEFINED, \
  .ptnstr = NULL, \
  .ptnint = 0, \
}

const static c2_l_t leaf_def = C2_L_INIT;

/// Linked list type of conditions.
struct _c2_lptr {
  c2_ptr_t ptr;
  void *data;
  struct _c2_lptr *next;
};

/// Initializer for c2_lptr_t.
#define C2_LPTR_INIT { \
  .ptr = C2_PTR_INIT, \
  .data = NULL, \
  .next = NULL, \
}

/// Structure representing a predefined target.
typedef struct {
  const char *name;
  enum c2_l_type type;
  int format;
} c2_predef_t;

// Predefined targets.
const static c2_predef_t C2_PREDEFS[] = {
  [C2_L_PID         ] = { "id"                , C2_L_TCARDINAL  , 0  },
  [C2_L_PX          ] = { "x"                 , C2_L_TCARDINAL  , 0  },
  [C2_L_PY          ] = { "y"                 , C2_L_TCARDINAL  , 0  },
  [C2_L_PX2         ] = { "x2"                , C2_L_TCARDINAL  , 0  },
  [C2_L_PY2         ] = { "y2"                , C2_L_TCARDINAL  , 0  },
  [C2_L_PWIDTH      ] = { "width"             , C2_L_TCARDINAL  , 0  },
  [C2_L_PHEIGHT     ] = { "height"            , C2_L_TCARDINAL  , 0  },
  [C2_L_PWIDTHB     ] = { "widthb"            , C2_L_TCARDINAL  , 0  },
  [C2_L_PHEIGHTB    ] = { "heightb"           , C2_L_TCARDINAL  , 0  },
  [C2_L_PBDW        ] = { "border_width"      , C2_L_TCARDINAL  , 0  },
  [C2_L_PFULLSCREEN ] = { "fullscreen"        , C2_L_TCARDINAL  , 0  },
  [C2_L_POVREDIR    ] = { "override_redirect" , C2_L_TCARDINAL  , 0  },
  [C2_L_PARGB       ] = { "argb"              , C2_L_TCARDINAL  , 0  },
  [C2_L_PFOCUSED    ] = { "focused"           , C2_L_TCARDINAL  , 0  },
  [C2_L_PWMWIN      ] = { "wmwin"             , C2_L_TCARDINAL  , 0  },
  [C2_L_PBSHAPED    ] = { "bounding_shaped"   , C2_L_TCARDINAL  , 0  },
  [C2_L_PROUNDED    ] = { "rounded_corners"   , C2_L_TCARDINAL  , 0  },
  [C2_L_PCLIENT     ] = { "client"            , C2_L_TWINDOW    , 0  },
  [C2_L_PWINDOWTYPE ] = { "window_type"       , C2_L_TSTRING    , 0  },
  [C2_L_PLEADER     ] = { "leader"            , C2_L_TWINDOW    , 0  },
  [C2_L_PNAME       ] = { "name"              , C2_L_TSTRING    , 0  },
  [C2_L_PCLASSG     ] = { "class_g"           , C2_L_TSTRING    , 0  },
  [C2_L_PCLASSI     ] = { "class_i"           , C2_L_TSTRING    , 0  },
  [C2_L_PROLE       ] = { "role"              , C2_L_TSTRING    , 0  },
};

#define mstrncmp(s1, s2) strncmp((s1), (s2), strlen(s1))

/**
 * Compare next word in a string with another string.
 */
static inline int
strcmp_wd(const char *needle, const char *src) {
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
static inline bool
c2_ptr_isempty(const c2_ptr_t p) {
  return !(p.isbranch ? (bool) p.b: (bool) p.l);
}

/**
 * Reset a c2_ptr_t.
 */
static inline void
c2_ptr_reset(c2_ptr_t *pp) {
  if (pp)
    memcpy(pp, &C2_PTR_NULL, sizeof(c2_ptr_t));
}

/**
 * Combine two condition trees.
 */
static inline c2_ptr_t
c2h_comb_tree(c2_b_op_t op, c2_ptr_t p1, c2_ptr_t p2) {
 c2_ptr_t p = {
   .isbranch = true,
   .b = malloc(sizeof(c2_b_t))
 };

 p.b->opr1 = p1;
 p.b->opr2 = p2;
 p.b->op = op;

 return p;
}

/**
 * Get the precedence value of a condition branch operator.
 */
static inline int
c2h_b_opp(c2_b_op_t op) {
  switch (op) {
    case C2_B_OAND:   return 2;
    case C2_B_OOR:    return 1;
    case C2_B_OXOR:   return 1;
    default:          break;
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
static inline int
c2h_b_opcmp(c2_b_op_t op1, c2_b_op_t op2) {
  return c2h_b_opp(op1) - c2h_b_opp(op2);
}

static int
c2_parse_grp(session_t *ps, const char *pattern, int offset, c2_ptr_t *presult, int level);

static int
c2_parse_target(session_t *ps, const char *pattern, int offset, c2_ptr_t *presult);

static int
c2_parse_op(const char *pattern, int offset, c2_ptr_t *presult);

static int
c2_parse_pattern(session_t *ps, const char *pattern, int offset, c2_ptr_t *presult);

static int
c2_parse_legacy(session_t *ps, const char *pattern, int offset, c2_ptr_t *presult);

static bool
c2_l_postprocess(session_t *ps, c2_l_t *pleaf);

static void
c2_free(c2_ptr_t p);

/**
 * Wrapper of c2_free().
 */
static inline void
c2_freep(c2_ptr_t *pp) {
  if (pp) {
    c2_free(*pp);
    c2_ptr_reset(pp);
  }
}

static const char *
c2h_dump_str_tgt(const c2_l_t *pleaf);

static const char *
c2h_dump_str_type(const c2_l_t *pleaf);

static void
c2_dump_raw(c2_ptr_t p);

/**
 * Wrapper of c2_dump_raw().
 */
static inline void
c2_dump(c2_ptr_t p) {
  c2_dump_raw(p);
  printf("\n");
  fflush(stdout);
}

static Atom
c2_get_atom_type(const c2_l_t *pleaf);

static bool
c2_match_once(session_t *ps, win *w, const c2_ptr_t cond);

