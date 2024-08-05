// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <uthash.h>

#include "curve.h"

#define OPERATORS                                                                        \
	X(OP_ADD)                                                                        \
	X(OP_SUB)                                                                        \
	X(OP_MUL)                                                                        \
	X(OP_DIV)                                                                        \
	/* Exponent */                                                                   \
	X(OP_EXP)                                                                        \
	/* Negation */                                                                   \
	X(OP_NEG)                                                                        \
	X(OP_MAX)

#define X(x) x,
enum op { OPERATORS };
#undef X

enum instruction_type {
	/// Push an immediate value to the top of the stack
	INST_IMM = 0,
	/// Pop two values from the top of the stack, apply operator,
	/// and push the result to the top of the stack
	INST_OP,
	/// Load a memory slot and push its value to the top of the stack.
	INST_LOAD,
	/// Load from evaluation context and push the value to the top of the stack.
	INST_LOAD_CTX,
	/// Pop one value from the top of the stack, and store it into a memory slot.
	INST_STORE,
	/// Pop one value from the top of the stack, if the memory slot contains NaN,
	/// store it into the memory slot; otherwise discard the value.
	INST_STORE_OVER_NAN,
	/// Pop a value from the top of the stack, clamp its value to [0, 1], then
	/// evaluate a curve at that point, push the result to the top of the stack.
	INST_CURVE,
	/// Jump to the branch target only when the script is evaluated for the first
	/// time. Used to perform initialization and such.
	INST_BRANCH_ONCE,
	/// Unconditional branch
	INST_BRANCH,
	INST_HALT,
};

/// Store metadata about where the result of a variable is stored
struct variable_allocation {
	UT_hash_handle hh;
	char *name;
	unsigned index;
	/// The memory slot for variable named `name`
	unsigned slot;
};

struct instruction {
	enum instruction_type type;
	union {
		double imm;
		enum op op;
		/// Memory slot for load and store
		unsigned slot;
		/// Context offset for load_ctx
		ptrdiff_t ctx;
		/// Relative PC change for branching
		int rel;
		/// The curve
		struct curve curve;
	};
};

/// When interrupting an already executing script and starting a new script,
/// we might want to inherit some of the existing values of variables as starting points,
/// i.e. we want to "resume" animation for the current state. This is configurable, and
/// can be disabled by enabling the `reset` property on a transition. This struct store
/// where the `start` variables of those "resumable" transition variables, which can be
/// overridden at the start of execution for this use case.
struct overridable_slot {
	UT_hash_handle hh;
	char *name;
	unsigned slot;
};

struct script {
	unsigned len;
	unsigned n_slots;
	/// The memory slot for storing the elapsed time.
	/// The next slot after this is used for storing the total duration of the script.
	unsigned elapsed_slot;
	unsigned stack_size;
	struct variable_allocation *vars;
	struct overridable_slot *overrides;
	struct instruction instrs[];
};
