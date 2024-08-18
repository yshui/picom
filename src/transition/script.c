// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <stdbool.h>
#include <stddef.h>

#include <libconfig.h>
#include <uthash.h>

#include "utils/dynarr.h"
#include "utils/list.h"
#include "utils/str.h"
#include "utils/uthash_extra.h"

#include "curve.h"
#include "script.h"
#include "script_internal.h"

#define X(x) [x] = #x,
static const char *op_names[] = {OPERATORS};
#undef X

struct fragment {
	struct list_node siblings;
	/// If `once` is true, this is the succeeding fragment if
	/// this fragment is executed.
	struct fragment *once_next;
	/// The succeeding fragment of this fragment. If `once` is true, this is the
	/// succeeding fragment if this fragment is NOT executed.
	struct fragment *next;
	/// Number of instructions
	unsigned ninstrs;
	/// The address of the first instruction of this fragment in compiled script.
	/// For `once` fragments, this is the address of the branch instruction.
	unsigned addr;
	/// Whether code is generated for this fragment. 0 = not emitted, 1 = emitted, 2 =
	/// in queue
	bool emitted;
	struct instruction instrs[];
};

/// Represent a variable during compilation. Contains a sequence of fragments, and
/// dependency of this variable.
struct compilation_stack {
	struct fragment *entry_point;
	struct fragment **exit;
	unsigned index;
	/// Number of dependencies
	unsigned ndeps;
	/// Whether the fragments loads from execution context
	bool need_context;
	/// Dependencies
	unsigned deps[];
};

struct script_compile_context {
	struct script_context_info_internal *context_info;
	struct variable_allocation *vars;
	struct overridable_slot *overrides;
	/// The memory slot for storing the elapsed time.
	/// The next slot after this is used for storing the total duration of the script.
	unsigned elapsed_slot;
	unsigned allocated_slots;
	unsigned max_stack;
	const char *current_variable_name;
	int *compiled;
	struct list_node all_fragments;
	struct fragment **tail, *head;
	/// Fragments that can be executed once at the beginning of the execution.
	/// For example, storing imms into memory slots.
	struct fragment **once_tail;
	/// Fragments that should be executed once at the end of the first execution.
	struct fragment **once_end_tail, *once_end_head;
};

static const char operators[] = "+-*/^";
static const enum op operator_types[] = {OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EXP};
static const int operator_pre[] = {0, 0, 1, 1, 2};

static void log_instruction_(enum log_level level, const char *func, unsigned index,
                             const struct instruction *inst) {
	if (log_get_level_tls() > level) {
		return;
	}
#define logv(fmt, ...) log_printf(tls_logger, level, func, "%u: " fmt, index, __VA_ARGS__)
	switch (inst->type) {
	case INST_IMM: logv("imm %f", inst->imm); break;
	case INST_BRANCH: logv("br %d", inst->rel); break;
	case INST_BRANCH_ONCE: logv("br_once %d", inst->rel); break;
	case INST_HALT: log_printf(tls_logger, level, func, "%u: halt", index); break;
	case INST_CURVE: log_printf(tls_logger, level, func, "%u: curve", index); break;
	case INST_OP: logv("op %s", op_names[inst->op]); break;
	case INST_LOAD: logv("load %u", inst->slot); break;
	case INST_STORE: logv("store %u", inst->slot); break;
	case INST_STORE_OVER_NAN: logv("store/nan %u", inst->slot); break;
	case INST_LOAD_CTX: logv("load_ctx *(%ld)", inst->ctx); break;
	}
#undef logv
}
#define log_instruction(level, i, inst)                                                  \
	log_instruction_(LOG_LEVEL_##level, __func__, i, &(inst))

char *instruction_to_c(struct instruction i) {
	char *buf = NULL;
	switch (i.type) {
	case INST_IMM: casprintf(&buf, "{.type = INST_IMM, .imm = %a},", i.imm); break;
	case INST_BRANCH:
		casprintf(&buf, "{.type = INST_BRANCH, .rel = %d},", i.rel);
		break;
	case INST_BRANCH_ONCE:
		casprintf(&buf, "{.type = INST_BRANCH_ONCE, .rel = %d},", i.rel);
		break;
	case INST_HALT: casprintf(&buf, "{.type = INST_HALT},"); break;
	case INST_CURVE:;
		char *curve = curve_to_c(&i.curve);
		casprintf(&buf, "{.type = INST_CURVE, .curve = %s},", curve);
		free(curve);
		break;
	case INST_OP:
		casprintf(&buf, "{.type = INST_OP, .op = %s},", op_names[i.op]);
		break;
	case INST_LOAD:
		casprintf(&buf, "{.type = INST_LOAD, .slot = %u},", i.slot);
		break;
	case INST_STORE:
		casprintf(&buf, "{.type = INST_STORE, .slot = %u},", i.slot);
		break;
	case INST_STORE_OVER_NAN:
		casprintf(&buf, "{.type = INST_STORE_OVER_NAN, .slot = %u},", i.slot);
		break;
	case INST_LOAD_CTX:
		casprintf(&buf, "{.type = INST_LOAD_CTX, .ctx = %ld},", i.ctx);
		break;
	}
	return buf;
}

static char parse_op(const char *input_str, const char **end, char **err) {
	char *op = strchr(operators, input_str[0]);
	*err = NULL;
	if (op != NULL) {
		*end = input_str + 1;
		return input_str[0];
	}

	casprintf(err, "Expected one of \"%s\", got '%c'.", operators, input_str[0]);
	*end = input_str;
	return 0;
}

static enum op char_to_op(char ch) {
	char *op = strchr(operators, ch);
	BUG_ON(op == NULL);
	return operator_types[op - operators];
}

struct script_context_info_internal {
	UT_hash_handle hh;
	struct script_context_info info;
};

struct expression_parser_context {
	char *op_stack;
	struct compilation_stack *entry;
	size_t len;

	unsigned op_top, operand_top;
	bool need_context;
};

/// Parse a number or a variable. Variable can optionally be prefixed with a minus sign.
static void
parse_raw_operand(struct expression_parser_context *ctx, const char *str, const char **end,
                  struct script_compile_context *script_ctx, char **err) {
	double number = strtod_simple(str, end);
	auto expr = ctx->entry->entry_point;
	*err = NULL;
	if (*end != str) {
		expr->instrs[expr->ninstrs++] = (struct instruction){
		    .type = INST_IMM,
		    .imm = number,
		};
		return;
	}
	bool neg = false;
	bool succeeded = false;
	if (**end == '-') {
		neg = true;
		str = skip_space(str + 1);
		*end = str;
	}
	while (**end) {
		if (isalnum(**end) || **end == '-' || **end == '_') {
			succeeded = true;
			(*end)++;
		} else {
			break;
		}
	}
	if (!succeeded) {
		casprintf(err, "Expected a number or a variable name, got \"%s\".", str);
		*end = str;
		return;
	}
	struct variable_allocation *var = NULL;
	struct script_context_info_internal *exe_ctx = NULL;
	HASH_FIND(hh, script_ctx->vars, str, (unsigned long)(*end - str), var);
	HASH_FIND(hh, script_ctx->context_info, str, (unsigned long)(*end - str), exe_ctx);
	if (var != NULL) {
		expr->instrs[expr->ninstrs++] = (struct instruction){
		    .type = INST_LOAD,
		    .slot = var->slot,
		};
		ctx->entry->deps[ctx->entry->ndeps++] = var->index;
	} else if (exe_ctx != NULL) {
		expr->instrs[expr->ninstrs++] = (struct instruction){
		    .type = INST_LOAD_CTX,
		    .ctx = exe_ctx->info.offset,
		};
		ctx->need_context = true;
	} else {
		casprintf(err, "variable name \"%.*s\" is not defined", (int)(*end - str), str);
		*end = str;
		return;
	}

	if (neg) {
		expr->instrs[expr->ninstrs++] = (struct instruction){
		    .type = INST_OP,
		    .op = OP_NEG,
		};
	}
}
static inline double op_eval(double l, enum op op, double r) {
	switch (op) {
	case OP_ADD: return l + r;
	case OP_SUB: return l - r;
	case OP_DIV: return l / r;
	case OP_MUL: return l * r;
	case OP_EXP: return pow(l, r);
	case OP_NEG: return -l;
	case OP_MAX: return max2(l, r);
	}
	unreachable();
}
static bool pop_op(const char *input_str, struct expression_parser_context *ctx, char **err) {
	if (ctx->operand_top < 2) {
		casprintf(err, "Missing operand for operator %c, in expression %s",
		          ctx->op_stack[ctx->op_top - 1], input_str);
		return false;
	}
	auto f = ctx->entry->entry_point;
	// Both operands are immediates, do constant propagation.
	if (f->instrs[f->ninstrs - 1].type == INST_IMM &&
	    f->instrs[f->ninstrs - 2].type == INST_IMM) {
		double imm = op_eval(f->instrs[f->ninstrs - 1].imm,
		                     char_to_op(ctx->op_stack[ctx->op_top]),
		                     f->instrs[f->ninstrs - 2].imm);
		ctx->operand_top -= 1;
		f->instrs[f->ninstrs - 2].imm = imm;
		f->ninstrs -= 1;
		ctx->op_top -= 1;
		return true;
	}
	f->instrs[f->ninstrs].type = INST_OP;
	f->instrs[f->ninstrs].op = char_to_op(ctx->op_stack[ctx->op_top - 1]);
	f->ninstrs += 1;
	ctx->op_top -= 1;
	ctx->operand_top -= 1;
	return true;
}

/// Parse an operand surrounded by some parenthesis:
///    `(((((var))` or `(((var` or `var)))`
static bool parse_operand_or_paren(struct expression_parser_context *ctx,
                                   const char *input_str, const char **end,
                                   struct script_compile_context *script_ctx, char **err) {
	const char *str = input_str;
	while (*str == '(') {
		str = skip_space(str + 1);
		ctx->op_stack[ctx->op_top++] = '(';
	}

	parse_raw_operand(ctx, str, end, script_ctx, err);
	if (str == *end) {
		return false;
	}
	str = skip_space(*end);
	ctx->operand_top += 1;
	if (ctx->operand_top > script_ctx->max_stack) {
		script_ctx->max_stack = ctx->operand_top;
	}

	while (*str == ')') {
		while (ctx->op_top > 0 && ctx->op_stack[ctx->op_top - 1] != '(') {
			if (!pop_op(str, ctx, err)) {
				return false;
			}
		}
		if (ctx->op_top == 0) {
			casprintf(err, "Unmatched ')' in expression \"%s\"", input_str);
			return false;
		}
		ctx->op_top -= 1;
		str = skip_space(str + 1);
	}
	*end = str;
	return true;
}

static struct fragment *fragment_new(struct script_compile_context *ctx, unsigned ninstrs) {
	struct fragment *fragment = calloc(
	    1, sizeof(struct fragment) + sizeof(struct instruction[max2(1, ninstrs)]));
	allocchk(fragment);
	list_insert_after(&ctx->all_fragments, &fragment->siblings);
	return fragment;
}

/// Precedence based expression parser. Prepend fragments to `stack_entry`, or allocate a
/// new one if `stack_entry` is NULL.
static bool expression_compile(struct compilation_stack **stack_entry, const char *input_str,
                               struct script_compile_context *script_ctx, unsigned slot,
                               bool allow_override, char **err) {
	const char *str = skip_space(input_str);
	const size_t len = strlen(str);
	BUG_ON(len > UINT_MAX);
	if (len == 0) {
		return false;
	}
	// At most each character in `input_str` could map to an individual instruction
	auto fragment = fragment_new(script_ctx, (unsigned)len + 1);
	if (!*stack_entry) {
		*stack_entry =
		    calloc(1, sizeof(struct compilation_stack) + sizeof(unsigned[len]));
		(*stack_entry)->exit = &fragment->next;
	} else {
		fragment->next = (*stack_entry)->entry_point;
	}
	(*stack_entry)->entry_point = fragment;

	struct expression_parser_context ctx = {
	    .op_stack = ccalloc(len, char),
	    .entry = *stack_entry,
	    .op_top = 0,
	    .operand_top = 0,
	};
	const char *end = NULL;
	bool succeeded = false;
	if (!parse_operand_or_paren(&ctx, str, &end, script_ctx, err)) {
		goto end;
	}

	str = end;
	while (*str) {
		str = skip_space(str);

		char new_op = parse_op(str, &end, err);
		if (str == end) {
			goto end;
		}
		str = skip_space(end);

		int pre = operator_pre[char_to_op(new_op)];
		while (ctx.op_top > 0 && ctx.op_stack[ctx.op_top - 1] != '(' &&
		       pre <= operator_pre[char_to_op(ctx.op_stack[ctx.op_top - 1])]) {
			if (!pop_op(input_str, &ctx, err)) {
				goto end;
			}
		}
		ctx.op_stack[ctx.op_top++] = new_op;
		if (!parse_operand_or_paren(&ctx, str, &end, script_ctx, err)) {
			goto end;
		}
		str = end;
	}
	while (ctx.op_top != 0) {
		if (!pop_op(input_str, &ctx, err)) {
			goto end;
		}
	}
	if (ctx.operand_top != 1) {
		casprintf(err, "excessive operand on stack %s", input_str);
		goto end;
	}
	succeeded = true;
	// Save the value of the expression
	// For overridable variables, we use store/nan, so caller can "pre-fill" the
	// variable to override it.
	fragment->instrs[fragment->ninstrs].type =
	    allow_override ? INST_STORE_OVER_NAN : INST_STORE;
	fragment->instrs[fragment->ninstrs++].slot = slot;
	(*stack_entry)->need_context = ctx.need_context;

end:
	free(ctx.op_stack);
	if (!succeeded) {
		free(*stack_entry);
		*stack_entry = NULL;
	}
	return succeeded;
}

static struct compilation_stack *
make_imm_stack_entry(struct script_compile_context *ctx, double imm, unsigned slot,
                     bool allow_override) {
	auto fragment = fragment_new(ctx, 2);
	fragment->ninstrs = 2;
	fragment->instrs[0].type = INST_IMM;
	fragment->instrs[0].imm = imm;
	fragment->instrs[1].type = allow_override ? INST_STORE_OVER_NAN : INST_STORE;
	fragment->instrs[1].slot = slot;
	*ctx->once_tail = fragment;
	ctx->once_tail = &fragment->next;

	// Insert an empty fragment for the stack entry
	fragment = fragment_new(ctx, 0);

	struct compilation_stack *entry = ccalloc(1, struct compilation_stack);
	allocchk(entry);
	entry->entry_point = fragment;
	entry->exit = &fragment->next;
	return entry;
}

static void compilation_stack_cleanup(struct compilation_stack **stack_entry) {
	free(*stack_entry);
	*stack_entry = NULL;
}

static bool
transition_compile(struct compilation_stack **stack_entry, config_setting_t *setting,
                   struct script_compile_context *ctx, unsigned slot, char **out_err) {
	int boolean = 0;
	double number = 0;
	struct curve curve;
	bool reset = false;
	char *err = NULL;
	const char *str = NULL;
	if (config_setting_lookup_string(setting, "curve", &str)) {
		curve = curve_parse(str, &str, &err);
		if (curve.type == CURVE_INVALID) {
			casprintf(out_err, "Cannot parse curve at line %d: %s",
			          config_setting_source_line(setting), err);
			free(err);
			return false;
		}
	} else if (config_setting_lookup(setting, "curve") != NULL) {
		casprintf(out_err, "Invalid curve definition at line %d. `curve` must be a string.",
		          config_setting_source_line(setting));
		return false;
	} else {
		curve = CURVE_LINEAR_INIT;
	}

	if (config_setting_lookup_bool(setting, "reset", &boolean)) {
		reset = boolean;
	}

	BUG_ON(ctx->allocated_slots > UINT_MAX - 1);

	// The start value must take a slot, because it's overridable.
	auto start_slot = ctx->allocated_slots;
	ctx->allocated_slots += 1;
	if (!reset) {
		auto override = ccalloc(1, struct overridable_slot);
		override->name = strdup(ctx->current_variable_name);
		override->slot = start_slot;
		HASH_ADD_STR(ctx->overrides, name, override);
	}
	cleanup(compilation_stack_cleanup) struct compilation_stack *start = NULL, *end = NULL;
	if (config_setting_lookup_float(setting, "start", &number)) {
		start = make_imm_stack_entry(ctx, number, start_slot, true);
	} else if (!config_setting_lookup_string(setting, "start", &str)) {
		casprintf(out_err,
		          "Transition definition does not contain a start value or "
		          "expression. Line %d.",
		          config_setting_source_line(setting));
		return false;
	} else if (!expression_compile(&start, str, ctx, start_slot, !reset, &err)) {
		casprintf(out_err, "transition has an invalid start expression: %s Line %d.",
		          err, config_setting_source_line(setting));
		free(err);
		return false;
	}

	// 0 = end, 1 = duration, 2 = delay
	struct instruction load_parameters[3];
	if (config_setting_lookup_float(setting, "end", &number)) {
		load_parameters[0] = (struct instruction){
		    .type = INST_IMM,
		    .imm = number,
		};
	} else if (!config_setting_lookup_string(setting, "end", &str)) {
		casprintf(out_err,
		          "Transition definition does not contain a end value or "
		          "expression. Line %d.",
		          config_setting_source_line(setting));
		return false;
	} else {
		BUG_ON(ctx->allocated_slots > UINT_MAX - 1);
		auto end_slot = ctx->allocated_slots++;
		if (!expression_compile(&end, str, ctx, end_slot, false, &err)) {
			casprintf(out_err,
			          "Transition has an invalid end expression: %s. Line %d",
			          err, config_setting_source_line(setting));
			free(err);
			return false;
		}
		load_parameters[0] = (struct instruction){
		    .type = INST_LOAD,
		    .slot = end_slot,
		};
	}

	if (config_setting_lookup_float(setting, "duration", &number)) {
		if (number == 0) {
			casprintf(out_err, "Duration must be greater than 0. Line %d.",
			          config_setting_source_line(setting));
			return false;
		}
		load_parameters[1] = (struct instruction){
		    .type = INST_IMM,
		    .imm = number,
		};
	} else if (!config_setting_lookup_string(setting, "duration", &str)) {
		casprintf(out_err,
		          "Transition definition does not contain a duration value or "
		          "expression. Line %d.",
		          config_setting_source_line(setting));
		return false;
	} else {
		BUG_ON(ctx->allocated_slots > UINT_MAX - 1);
		auto duration_slot = ctx->allocated_slots++;
		if (!expression_compile(&end, str, ctx, duration_slot, false, &err)) {
			casprintf(out_err,
			          "Transition has an invalid duration expression: %s. "
			          "Line %d",
			          err, config_setting_source_line(setting));
			free(err);
			return false;
		}
		load_parameters[1] = (struct instruction){
		    .type = INST_LOAD,
		    .slot = duration_slot,
		};
	}

	if (config_setting_lookup_float(setting, "delay", &number)) {
		load_parameters[2] = (struct instruction){
		    .type = INST_IMM,
		    .imm = number,
		};
	} else if (!config_setting_lookup_string(setting, "delay", &str)) {
		load_parameters[2] = (struct instruction){
		    .type = INST_IMM,
		    .imm = 0,
		};
	} else {
		BUG_ON(ctx->allocated_slots > UINT_MAX - 1);
		auto delay_slot = ctx->allocated_slots++;
		if (!expression_compile(&end, str, ctx, delay_slot, false, &err)) {
			casprintf(out_err, "Transition has an invalid delay expression: %s. Line %d",
			          err, config_setting_source_line(setting));
			free(err);
			return false;
		}
		load_parameters[2] = (struct instruction){
		    .type = INST_LOAD,
		    .slot = delay_slot,
		};
	}

	// clang-format off
	struct instruction instrs[] = {
	    load_parameters[0],
	    {.type = INST_LOAD, .slot = start_slot},
	    {.type = INST_OP, .op = OP_SUB},         // v0 = end - start
	    {.type = INST_LOAD, .slot = ctx->elapsed_slot},
	    load_parameters[2],
	    {.type = INST_OP, .op = OP_SUB},         // v1 = elapsed - delay
	    load_parameters[1],
	    {.type = INST_OP, .op = OP_DIV},         // v2 = v1 / duration
	    {.type = INST_CURVE, .curve = curve},    // v3 = curve(v2)
	    {.type = INST_OP, .op = OP_MUL},         // v4 = v0 * v3
	    {.type = INST_LOAD, .slot = start_slot},
	    {.type = INST_OP, .op = OP_ADD},         // v5 = v4 + start
	    {.type = INST_STORE, .slot = slot},      // memory[slot] = v5
	};

	// Instructs for calculating the total duration of the transition
	struct instruction total_duration_instrs[] = {
	    load_parameters[1],
	    load_parameters[2],
	    {.type = INST_OP, .op = OP_ADD},        // v0 = duration + delay
	    {.type = INST_LOAD, .slot = ctx->elapsed_slot + 1},
	    {.type = INST_OP, .op = OP_MAX},        // v1 = max(v0, total_duration)
	    {.type = INST_STORE, .slot = ctx->elapsed_slot + 1},
	};
	// clang-format on

	if (ctx->max_stack < 3) {
		// The list of instructions above needs 3 stack slots
		ctx->max_stack = 3;
	}

	struct fragment *fragment = fragment_new(ctx, ARR_SIZE(instrs));
	memcpy(fragment->instrs, instrs, sizeof(instrs));
	fragment->ninstrs = ARR_SIZE(instrs);

	*stack_entry = calloc(
	    1, sizeof(struct compilation_stack) + sizeof(unsigned[max2(1, start->ndeps)]));
	allocchk(*stack_entry);
	struct fragment **next = &(*stack_entry)->entry_point;

	// Dependencies of the start value is the real dependencies of this transition
	// variable.
	(*stack_entry)->ndeps = start->ndeps;
	// If start value has dependencies, we calculate it like this:
	//    if (first_evaluation) mem[start_slot] = <start expression>;
	// Otherwise, it's put into the "evaluation once" block.
	if (start->ndeps > 0) {
		memcpy((*stack_entry)->deps, start->deps, sizeof(unsigned[start->ndeps]));

		auto branch = fragment_new(ctx, 0);
		*next = branch;
		branch->once_next = start->entry_point;

		auto phi = fragment_new(ctx, 0);
		*start->exit = phi;
		branch->next = phi;
		next = &phi->next;
	} else {
		*ctx->once_tail = start->entry_point;
		ctx->once_tail = start->exit;
	}

	// The `end` block includes `end`, `duration`, and `delay` values.
	if (end != NULL && end->ndeps > 0) {
		// If we get here, the end/duration/delay values are not static, luckily
		// we can still just calculate it at the end of the first evaluation,
		// since at that point nothing can depends on a transition's end value.
		// However, for the calculation of this transition curve, we don't yet
		// have the these values, so we do this: `mem[output_slot] =
		// mem[start_slot]`, instead of compute it normally.
		*ctx->once_end_tail = end->entry_point;
		ctx->once_end_tail = end->exit;

		const struct instruction load_store_instrs[] = {
		    {.type = INST_LOAD, .slot = start_slot},
		    {.type = INST_STORE, .slot = slot},
		};
		auto load_store = fragment_new(ctx, ARR_SIZE(load_store_instrs));
		load_store->ninstrs = ARR_SIZE(load_store_instrs);
		memcpy(load_store->instrs, load_store_instrs, sizeof(load_store_instrs));

		auto branch = fragment_new(ctx, 0);
		*next = branch;
		branch->once_next = load_store;
		branch->next = fragment;

		auto phi = fragment_new(ctx, 0);
		load_store->next = phi;
		fragment->next = phi;
		(*stack_entry)->exit = &phi->next;
	} else {
		if (end != NULL) {
			// The end value has no dependencies, so it only needs to be
			// evaluated once at the start of the first evaluation. And
			// therefore we can evaluate the curve like normal even for the
			// first evaluation.
			*ctx->once_tail = end->entry_point;
			ctx->once_tail = end->exit;
		}

		*next = fragment;
		(*stack_entry)->exit = &fragment->next;
	}

	// This must happen _after_ the `end` block.
	struct fragment *total_duration_fragment = fragment_new(ctx, ARR_SIZE(instrs));
	memcpy(total_duration_fragment->instrs, total_duration_instrs,
	       sizeof(total_duration_instrs));
	total_duration_fragment->ninstrs = ARR_SIZE(total_duration_instrs);
	*ctx->once_end_tail = total_duration_fragment;
	ctx->once_end_tail = &total_duration_fragment->next;

	return true;
}

static void instruction_deinit(struct instruction * /*instr*/) {
}

static void fragment_free(struct fragment *frag) {
	list_remove(&frag->siblings);
	for (unsigned i = 0; i < frag->ninstrs; i++) {
		instruction_deinit(&frag->instrs[i]);
	}
	free(frag);
}

#define free_hash_table(head)                                                            \
	do {                                                                             \
		typeof(head) i, ni;                                                      \
		HASH_ITER(hh, head, i, ni) {                                             \
			HASH_DEL(head, i);                                               \
			free(i->name);                                                   \
			free(i);                                                         \
		}                                                                        \
	} while (0)

void script_free(struct script *script) {
	for (unsigned i = 0; i < script->len; i++) {
		instruction_deinit(&script->instrs[i]);
	}
	free_hash_table(script->vars);
	free_hash_table(script->overrides);
	free(script);
}

static bool script_compile_one(struct compilation_stack **stack_entry, config_setting_t *var,
                               struct script_compile_context *ctx, char **err) {
	ctx->current_variable_name = config_setting_name(var);

	struct variable_allocation *alloc = NULL;
	HASH_FIND_STR(ctx->vars, ctx->current_variable_name, alloc);
	BUG_ON(!alloc);

	if (config_setting_is_number(var)) {
		*stack_entry = make_imm_stack_entry(ctx, config_setting_get_float(var),
		                                    alloc->slot, false);
		return true;
	}
	const char *str = config_setting_get_string(var);
	if (str != NULL) {
		char *tmp_err = NULL;
		bool succeeded =
		    expression_compile(stack_entry, str, ctx, alloc->slot, false, &tmp_err);
		if (!succeeded) {
			casprintf(err, "Failed to parse expression at line %d. %s",
			          config_setting_source_line(var), tmp_err);
			free(tmp_err);
		}
		return succeeded;
	}

	if (!config_setting_is_group(var)) {
		casprintf(err,
		          "Invalid variable \"%s\", it must be either a number, a "
		          "string, "
		          "or a config group defining a transition.",
		          config_setting_name(var));
		return false;
	}
	return transition_compile(stack_entry, var, ctx, alloc->slot, err);
}

static void report_cycle(struct compilation_stack **stack, unsigned top, unsigned index,
                         config_setting_t *setting, char **err) {
	unsigned start = top - 1;
	while (stack[start]->index != index) {
		start -= 1;
	}
	auto last_var = config_setting_get_elem(setting, index);
	auto last_name = config_setting_name(last_var);
	auto len = (size_t)(top - start) * 4 /* " -> " */ + strlen(last_name);
	for (unsigned i = start; i < top; i++) {
		auto var = config_setting_get_elem(setting, stack[i]->index);
		len += strlen(config_setting_name(var));
	}
	auto buf = ccalloc(len + 1, char);
	auto pos = buf;
	for (unsigned i = start; i < top; i++) {
		auto var = config_setting_get_elem(setting, stack[i]->index);
		auto name = config_setting_name(var);
		strcpy(pos, name);
		pos += strlen(name);
		strcpy(pos, " -> ");
		pos += 4;
	}
	strcpy(pos, last_name);

	casprintf(err, "Cyclic references detected in animation script defined at line %d: %s",
	          config_setting_source_line(setting), buf);
	free(buf);
}

static bool script_compile_one_recursive(struct compilation_stack **stack,
                                         config_setting_t *setting, unsigned index,
                                         struct script_compile_context *ctx, char **err) {
	unsigned stack_top = 1;
	if (!script_compile_one(&stack[0], config_setting_get_elem(setting, index), ctx, err)) {
		return false;
	}
	stack[0]->index = index;
	ctx->compiled[index] = 2;
	while (stack_top) {
		auto stack_entry = stack[stack_top - 1];
		while (stack_entry->ndeps) {
			auto dep = stack_entry->deps[--stack_entry->ndeps];
			if (ctx->compiled[dep] == 1) {
				continue;
			}
			if (ctx->compiled[dep] == 2) {
				report_cycle(stack, stack_top, dep, setting, err);
				goto out;
			}

			auto dep_setting = config_setting_get_elem(setting, dep);
			if (!script_compile_one(&stack[stack_top], dep_setting, ctx, err)) {
				goto out;
			}
			stack[stack_top]->index = dep;
			ctx->compiled[dep] = 2;
			stack_top += 1;
			goto next;
		}
		// Top of the stack has all of its dependencies resolved, we can emit
		// its fragment.
		*ctx->tail = stack_entry->entry_point;
		ctx->tail = stack_entry->exit;
		ctx->compiled[stack_entry->index] = 1;
		stack[--stack_top] = NULL;
		free(stack_entry);
	next:;
	}
out:
	for (unsigned i = 0; i < stack_top; i++) {
		free(stack[i]);
	}
	return stack_top == 0;
}

static void prune_fragments(struct list_node *head) {
	bool changed = true;
	while (changed) {
		changed = false;
		list_foreach(struct fragment, i, head, siblings) {
			if (i->once_next == i->next && i->next != NULL) {
				i->once_next = NULL;
				changed = true;
			}
		}
		list_foreach(struct fragment, i, head, siblings) {
			struct fragment *non_empty = i->next;
			while (non_empty && non_empty->ninstrs == 0 &&
			       non_empty->once_next == NULL) {
				non_empty = non_empty->next;
			}
			changed |= (non_empty != i->next);
			i->next = non_empty;
			non_empty = i->once_next;
			while (non_empty && non_empty->ninstrs == 0 &&
			       non_empty->once_next == NULL) {
				non_empty = non_empty->next;
			}
			changed |= (non_empty != i->once_next);
			i->once_next = non_empty;
		}
	}
}

static struct script *script_codegen(struct list_node *all_fragments, struct fragment *head) {
	unsigned nfragments = 0;
	list_foreach(struct fragment, i, all_fragments, siblings) {
		nfragments += 1;
	}
	auto queue = ccalloc(nfragments, struct fragment *);
	unsigned pos = 0, h = 0, t = 1;
	queue[0] = head;
	head->emitted = true;
	// First, layout the fragments in the output
	while (h != t) {
		auto curr = queue[h];
		while (curr) {
			curr->addr = pos;
			curr->emitted = true;
			pos += curr->ninstrs;
			if (curr->once_next) {
				pos += 1;        // For branch_once
				if (!curr->once_next->emitted) {
					queue[t++] = curr->once_next;
					curr->once_next->emitted = true;
				}
			}
			if ((curr->next && curr->next->emitted) || !curr->next) {
				pos += 1;        // For branch or halt
				break;
			}
			curr = curr->next;
		}
		h += 1;
	}
	struct script *script =
	    calloc(1, sizeof(struct script) + sizeof(struct instruction[pos]));
	script->len = pos;
	free(queue);

	list_foreach(const struct fragment, i, all_fragments, siblings) {
		if (i->ninstrs) {
			memcpy(&script->instrs[i->addr], i->instrs,
			       sizeof(struct instruction[i->ninstrs]));
		}

		auto ninstrs = i->ninstrs;
		if (i->once_next) {
			script->instrs[i->addr + ninstrs].type = INST_BRANCH_ONCE;
			script->instrs[i->addr + ninstrs].rel =
			    (int)i->once_next->addr - (int)(i->addr + ninstrs);
			ninstrs += 1;
		}
		if (i->next && i->next->addr != i->addr + ninstrs) {
			script->instrs[i->addr + ninstrs].type = INST_BRANCH;
			script->instrs[i->addr + ninstrs].rel =
			    (int)i->next->addr - (int)(i->addr + ninstrs);
		} else if (!i->next) {
			script->instrs[i->addr + ninstrs].type = INST_HALT;
		}
	}
	return script;
}

static void
script_compile_context_init(struct script_compile_context *ctx, config_setting_t *setting) {
	list_init_head(&ctx->all_fragments);
	const uint32_t n = to_u32_checked(config_setting_length(setting));
	ctx->compiled = ccalloc(n, int);
	for (uint32_t i = 0; i < n; i++) {
		auto var = config_setting_get_elem(setting, i);
		const char *var_name = config_setting_name(var);
		auto alloc = ccalloc(1, struct variable_allocation);
		alloc->name = strdup(var_name);
		alloc->index = i;
		alloc->slot = i;
		HASH_ADD_STR(ctx->vars, name, alloc);
	}

	ctx->allocated_slots = n + 2;
	ctx->elapsed_slot = n;

	auto head = fragment_new(ctx, 0);
	ctx->head = head;
	ctx->once_tail = &head->once_next;
	ctx->tail = &head->next;

	ctx->once_end_head = fragment_new(ctx, 2);
	ctx->once_end_head->instrs[0] = (struct instruction){
	    .type = INST_IMM,
	    .imm = 0,
	};
	ctx->once_end_head->instrs[1] = (struct instruction){
	    .type = INST_STORE,
	    .slot = ctx->elapsed_slot + 1,
	};
	ctx->once_end_head->ninstrs = 2;
	ctx->once_end_tail = &ctx->once_end_head->next;
	ctx->max_stack = 1;
}

unsigned script_elapsed_slot(const struct script *script) {
	return script->elapsed_slot;
}

unsigned script_total_duration_slot(const struct script *script) {
	return script->elapsed_slot + 1;
}

struct script *
script_compile(config_setting_t *setting, struct script_parse_config cfg, char **out_err) {
	if (!config_setting_is_group(setting)) {
		casprintf(out_err, "Script setting must be a group");
		return NULL;
	}
	struct script_context_info_internal *context_table = NULL;
	if (cfg.context_info) {
		for (unsigned i = 0; cfg.context_info[i].name; i++) {
			struct script_context_info_internal *new_ctx =
			    ccalloc(1, struct script_context_info_internal);
			new_ctx->info = cfg.context_info[i];
			HASH_ADD_STR(context_table, info.name, new_ctx);
		}
	}

	struct script_compile_context ctx = {};
	script_compile_context_init(&ctx, setting);
	ctx.context_info = context_table;
	const uint32_t n = to_u32_checked(config_setting_length(setting));
	auto stack = ccalloc(n, struct compilation_stack *);
	for (uint32_t i = 0; i < n; i++) {
		if (ctx.compiled[i]) {
			continue;
		}
		if (!script_compile_one_recursive(stack, setting, i, &ctx, out_err)) {
			break;
		}
	}

	{
		struct script_context_info_internal *info, *next_info;
		HASH_ITER(hh, context_table, info, next_info) {
			HASH_DEL(context_table, info);
			free(info);
		}
	}
	for (int i = 0; cfg.output_info && cfg.output_info[i].name; i++) {
		struct variable_allocation *alloc = NULL;
		HASH_FIND_STR(ctx.vars, cfg.output_info[i].name, alloc);
		if (alloc) {
			cfg.output_info[i].slot = to_int_checked(alloc->slot);
		} else {
			cfg.output_info[i].slot = -1;
		}
	}

	bool succeeded = true;
	for (unsigned i = 0; i < n; i++) {
		if (ctx.compiled[i] != 1) {
			succeeded = false;
			break;
		}
	}
	free(stack);
	free(ctx.compiled);
	if (!succeeded) {
		free_hash_table(ctx.vars);
		free_hash_table(ctx.overrides);
		list_foreach_safe(struct fragment, i, &ctx.all_fragments, siblings) {
			fragment_free(i);
		}
		return NULL;
	}

	// Connect everything together
	if (ctx.once_end_head) {
		auto once_end = fragment_new(&ctx, 0);
		*ctx.tail = once_end;
		once_end->once_next = ctx.once_end_head;
	}
	*ctx.once_tail = ctx.head->next;

	prune_fragments(&ctx.all_fragments);

	auto script = script_codegen(&ctx.all_fragments, ctx.head);
	script->vars = ctx.vars;
	script->overrides = ctx.overrides;
	script->elapsed_slot = ctx.elapsed_slot;
	script->n_slots = ctx.allocated_slots;
	script->stack_size = ctx.max_stack;
	log_debug("Compiled script at line %d, total instructions: %d, "
	          "slots: %d, stack size: %d, memory[%u] = total duration, memory[%u] = "
	          "elapsed",
	          config_setting_source_line(setting), script->len, script->n_slots,
	          script->stack_size, script->elapsed_slot + 1, script->elapsed_slot);
	if (log_get_level_tls() <= LOG_LEVEL_DEBUG) {
		log_debug("Output mapping:");
		HASH_ITER2(ctx.vars, var) {
			log_debug("    %s -> %d", var->name, var->slot);
		}
	}
	if (log_get_level_tls() <= LOG_LEVEL_TRACE) {
		for (unsigned i = 0; i < script->len; i++) {
			log_instruction(TRACE, i, script->instrs[i]);
		}
	}
	list_foreach_safe(struct fragment, i, &ctx.all_fragments, siblings) {
		free(i);
	}
	return script;
}

char *script_to_c(const struct script *script, const struct script_output_info *outputs) {
	char **buf = dynarr_new(char *, script->len * 40);
	char *tmp = NULL;
	casprintf(&tmp, "{\n"
	                "    static const struct instruction instrs[] = {\n");
	dynarr_push(buf, tmp);
	for (unsigned i = 0; i < script->len; i++) {
		dynarr_push(buf, instruction_to_c(script->instrs[i]));
	}
	casprintf(&tmp,
	          "    };\n    struct script *ret = \n"
	          "    malloc(offsetof(struct script, instrs) + sizeof(instrs));\n"
	          "    ret->len = ARR_SIZE(instrs); ret->elapsed_slot = %u;"
	          "    ret->n_slots = %u; ret->stack_size = %u;\n"
	          "    ret->vars = NULL; ret->overrides = NULL;\n"
	          "    memcpy(ret->instrs, instrs, sizeof(instrs));\n",
	          script->elapsed_slot, script->n_slots, script->stack_size);
	dynarr_push(buf, tmp);

	struct variable_allocation *var, *next_var;
	HASH_ITER(hh, script->vars, var, next_var) {
		char *var_str = NULL;
		casprintf(&var_str,
		          "    {\n"
		          "      struct variable_allocation *var = \n"
		          "          malloc(sizeof(*var));\n"
		          "      *var = (struct variable_allocation){\n"
		          "          .name = strdup(\"%s\"), .slot = %u, .index = %u\n"
		          "      };\n"
		          "      HASH_ADD_STR(ret->vars, name, var);\n"
		          "    }\n",
		          var->name, var->slot, var->index);
		dynarr_push(buf, var_str);
	}

	struct overridable_slot *override, *next_override;
	HASH_ITER(hh, script->overrides, override, next_override) {
		char *override_str = NULL;
		casprintf(&override_str,
		          "    {\n"
		          "      struct overridable_slot *override = \n"
		          "         malloc(sizeof(*override));\n"
		          "      *override = (struct overridable_slot){\n"
		          "          .name = strdup(\"%s\"), .slot = %u\n"
		          "      };\n"
		          "      HASH_ADD_STR(ret->overrides, name, override);\n"
		          "    }\n",
		          override->name, override->slot);
		dynarr_push(buf, override_str);
	}

	for (size_t i = 0; outputs && outputs[i].name; i++) {
		struct variable_allocation *alloc = NULL;
		HASH_FIND_STR(script->vars, outputs[i].name, alloc);
		if (alloc) {
			casprintf(&tmp, "    output_slots[%zu] = %u;\n", i, alloc->slot);
		} else {
			casprintf(&tmp, "    output_slots[%zu] = -1;\n", i);
		}
		dynarr_push(buf, tmp);
	}

	casprintf(&tmp, "    return ret;\n}\n");
	dynarr_push(buf, tmp);
	return dynarr_join(buf, "");
}

void script_specialize(struct script *script,
                       const struct script_specialization_context *spec, unsigned n_context) {
	for (unsigned i = 0; i < script->len; i++) {
		if (script->instrs[i].type != INST_LOAD_CTX) {
			continue;
		}
		for (unsigned j = 0; j < n_context; j++) {
			if (script->instrs[i].ctx == spec[j].offset) {
				script->instrs[i].type = INST_IMM;
				script->instrs[i].imm = spec[j].value;
				break;
			}
		}
	}
}

struct script_instance *script_instance_new(const struct script *script) {
	// allocate no space for the variable length array is UB.
	unsigned memory_size = max2(1, script->n_slots + script->stack_size);
	struct script_instance *instance =
	    calloc(1, sizeof(struct script_instance) + sizeof(double[memory_size]));
	allocchk(instance);
	instance->script = script;
	for (unsigned i = 0; i < script->n_slots; i++) {
		instance->memory[i] = NAN;
	}
	instance->memory[script->elapsed_slot] = 0;
	return instance;
}

void script_instance_resume_from(struct script_instance *old, struct script_instance *new_) {
	// todo: proper steer logic
	struct overridable_slot *i, *next;
	HASH_ITER(hh, new_->script->overrides, i, next) {
		struct variable_allocation *src_alloc = NULL;
		HASH_FIND_STR(old->script->vars, i->name, src_alloc);
		if (!src_alloc) {
			continue;
		}
		new_->memory[i->slot] = old->memory[src_alloc->slot];
	}
}

enum script_evaluation_result
script_instance_evaluate(struct script_instance *instance, void *context) {
	auto script = instance->script;
	auto stack = (double *)&instance->memory[script->n_slots];
	unsigned top = 0;
	double l, r;
	bool do_branch_once = instance->memory[script->elapsed_slot] == 0;
	for (auto i = script->instrs;; i++) {
		switch (i->type) {
		case INST_IMM: stack[top++] = i->imm; break;
		case INST_LOAD: stack[top++] = instance->memory[i->slot]; break;
		case INST_LOAD_CTX: stack[top++] = *(double *)(context + i->ctx); break;
		case INST_STORE:
			BUG_ON(top < 1);
			instance->memory[i->slot] = stack[--top];
			break;
		case INST_STORE_OVER_NAN:
			BUG_ON(top < 1);
			top -= 1;
			if (safe_isnan(instance->memory[i->slot])) {
				instance->memory[i->slot] = stack[top];
			}
			break;
		case INST_BRANCH: i += i->rel - 1; break;
		case INST_BRANCH_ONCE:
			if (do_branch_once) {
				i += i->rel - 1;
			}
			break;
		case INST_HALT: return SCRIPT_EVAL_OK;
		case INST_OP:
			if (i->op == OP_NEG) {
				BUG_ON(top < 1);
				l = stack[top - 1];
				stack[top - 1] = -l;
			} else {
				BUG_ON(top < 2);
				l = stack[top - 2];
				r = stack[top - 1];
				stack[top - 2] = op_eval(l, i->op, r);
				top -= 1;
			}
			break;
		case INST_CURVE:
			BUG_ON(top < 1);
			l = stack[top - 1];
			l = min2(max2(0, l), 1);
			stack[top - 1] = curve_sample(&i->curve, l);
			break;
		}
		if (top && safe_isnan(stack[top - 1])) {
			return SCRIPT_EVAL_ERROR_NAN;
		}
		if (top && safe_isinf(stack[top - 1])) {
			return SCRIPT_EVAL_ERROR_INF;
		}
	}
	unreachable();
}

#ifdef UNIT_TEST
static inline void
script_compile_str(struct test_case_metadata *metadata, const char *str,
                   struct script_output_info *outputs, char **err, struct script **out) {
	config_t cfg;
	config_init(&cfg);
	config_set_auto_convert(&cfg, 1);
	int ret = config_read_string(&cfg, str);
	TEST_EQUAL(ret, CONFIG_TRUE);

	config_setting_t *setting = config_root_setting(&cfg);
	TEST_NOTEQUAL(setting, NULL);
	*out = script_compile(setting,
	                      (struct script_parse_config){.output_info = outputs}, err);
	config_destroy(&cfg);
}

TEST_CASE(scripts_1) {
	static const char *str = "\
		a = 10; \
		b = \"a * 2\";\
		c = \"(b - 1) * (a+1)\";\
		d = \"- e - 1\"; \
		e : { \
			curve = \"cubic-bezier(0.5,0.5, 0.5, 0.5)\"; \
			duration = \"a\"; \
			delay = 0.5; \
			start = 10; \
			end = \"2 * c\"; \
		}; \
		f : { \
			curve = \"cubic-bezier(0.1,0.2, 0.3, 0.4)\"; \
			duration = 10; \
			delay = 0.5; \
			start = \"e + 1\"; \
			end = \"f - 1\"; \
		}; \
		neg = \"-a\"; \
		timing1 : { \
			duration = 10; \
			start = 1; \
			end = 0; \
		};\
		timing2 : { \
			curve = \"steps(1, jump-start)\"; \
			duration = 10; \
			start = 1; \
			end = 0; \
		};";
	struct script_output_info outputs[] = {{"a"}, {"b"}, {"c"}, {"d"}, {"e"}, {NULL}};
	char *err = NULL;
	struct script *script = NULL;
	script_compile_str(metadata, str, outputs, &err, &script);
	if (err) {
		log_error("err: %s\n", err);
		free(err);
	}
	TEST_NOTEQUAL(script, NULL);
	TEST_EQUAL(err, NULL);
	if (script) {
		struct variable_allocation *c;
		HASH_FIND_STR(script->vars, "c", c);
		TEST_NOTEQUAL(c, NULL);

		struct script_instance *instance = script_instance_new(script);
		auto result = script_instance_evaluate(instance, NULL);
		TEST_EQUAL(result, SCRIPT_EVAL_OK);
		TEST_EQUAL(instance->memory[script->elapsed_slot + 1], 10.5);
		TEST_EQUAL(instance->memory[outputs[0].slot], 10);
		TEST_EQUAL(instance->memory[outputs[1].slot], 20);
		TEST_EQUAL(instance->memory[outputs[2].slot], 209);
		TEST_EQUAL(instance->memory[outputs[3].slot], -11);
		TEST_EQUAL(instance->memory[outputs[4].slot], 10);
		TEST_TRUE(!script_instance_is_finished(instance));

		instance->memory[instance->script->elapsed_slot] += 5.5;
		result = script_instance_evaluate(instance, NULL);
		TEST_EQUAL(result, SCRIPT_EVAL_OK);
		TEST_EQUAL(instance->memory[outputs[4].slot], 214);

		instance->memory[instance->script->elapsed_slot] += 5.5;
		result = script_instance_evaluate(instance, NULL);
		TEST_EQUAL(result, SCRIPT_EVAL_OK);
		TEST_EQUAL(instance->memory[outputs[0].slot], 10);
		TEST_EQUAL(instance->memory[outputs[1].slot], 20);
		TEST_EQUAL(instance->memory[outputs[2].slot], 209);
		TEST_EQUAL(instance->memory[outputs[3].slot], -419);
		TEST_EQUAL(instance->memory[outputs[4].slot], 418);
		TEST_TRUE(script_instance_is_finished(instance));
		free(instance);
		script_free(script);
	}
}
TEST_CASE(scripts_report_cycle) {
	static const char *str = "\
		a = \"c\"; \
		b = \"a * 2\";\
		c = \"b + 1\";";
	char *err = NULL;
	struct script *script = NULL;
	script_compile_str(metadata, str, NULL, &err, &script);
	TEST_EQUAL(script, NULL);
	TEST_NOTEQUAL(err, NULL);
	TEST_STREQUAL(err, "Cyclic references detected in animation script defined at "
	                   "line 0: a -> c -> b -> a");
	free(err);
}
TEST_CASE(script_errors) {
	static const char *cases[][2] = {
	    {"a = \"1 @ 2 \";", "Failed to parse expression at line 1. Expected one of "
	                        "\"+-*/^\", got '@'."},
	    {"a = { curve = \"asdf\";};", "Cannot parse curve at line 1: Unknown curve "
	                                  "type \"asdf\"."},
	    {"a = { curve = \"steps(a)\";};", "Cannot parse curve at line 1: Invalid "
	                                      "step count at \"a)\"."},
	    {"a = { curve = \"steps(1)\";};", "Cannot parse curve at line 1: Invalid "
	                                      "steps argument list \"(1)\"."},
	    {"a = \"1 + +\";", "Failed to parse expression at line 1. Expected a number "
	                       "or a variable name, got \"+\"."},
	    {"a = \"1)\";", "Failed to parse expression at line 1. Unmatched ')' in "
	                    "expression \"1)\""},
	    {"a = {};", "Transition definition does not contain a start value or "
	                "expression. Line 1."},
	    {"a = { duration = 0; start = 0; end = 0; };", "Duration must be greater "
	                                                   "than 0. Line 1."},
	};
	char *err = NULL;
	struct script *script = NULL;
	for (size_t i = 0; i < ARR_SIZE(cases); i++) {
		script_compile_str(metadata, cases[i][0], NULL, &err, &script);
		TEST_EQUAL(script, NULL);
		TEST_NOTEQUAL(err, NULL);
		TEST_STREQUAL(err, cases[i][1]);
		free(err);
		err = NULL;
	}
}
#endif
