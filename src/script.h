#pragma once
#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <uthash.h>

struct script_context_info {
	const char *name;
	ptrdiff_t offset;
};

struct script_output_info {
	const char *name;
	/// Slot for this variable, -1 if this variable doesn't exist.
	int slot;
};

struct script_parse_config {
	const struct script_context_info *context_info;
	/// Set the output variables of this script, also used to receive the slot number
	/// for those variables.
	struct script_output_info *output_info;
};
struct script;
struct script_instance {
	const struct script *script;
	double elapsed;
	double memory[];
};
enum script_evaluation_result {
	/// +/-inf in results
	SCRIPT_EVAL_ERROR_INF,
	/// NaN in results
	SCRIPT_EVAL_ERROR_NAN,
	/// OK
	SCRIPT_EVAL_OK,
};
typedef struct config_setting_t config_setting_t;
static_assert(alignof(double) > alignof(unsigned), "double/unsigned has unexpected "
                                                   "alignment");

struct script *
script_compile(config_setting_t *setting, struct script_parse_config cfg, char **out_err);
void script_free(struct script *script);
enum script_evaluation_result
script_instance_evaluate(struct script_instance *instance, void *context);
bool script_instance_is_finished(const struct script_instance *instance);
/// Resume the script instance from another script instance that's currently running.
/// The script doesn't have to be the same. For resumable (explained later) transitions,
/// if matching variables exist in the `old` script, their starting point will be
/// overridden with the current value of matching variables from `old`. A resumable
/// transition is a transition that will "resume" from wherever its current value is.
/// Imagine a window flying off the screen, for some reason you decided to bring it back
/// when it's just halfway cross. It would be jarring if the window jumps, so you would
/// want it to fly back in from where it currently is, instead from out of the screen.
/// This resuming behavior can be turned off by setting `reset = true;` in the transition
/// configuration, in which case the user defined `start` value will always be used.
void script_instance_resume_from(struct script_instance *old, struct script_instance *new_);
struct script_instance *script_instance_new(const struct script *script);
