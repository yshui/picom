// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include <picom/api.h>
#include <picom/backend.h>

#include <uthash.h>

#include "api_internal.h"
#include "compiler.h"
#include "log.h"
#include "transition/script.h"
#include "utils/dynarr.h"
#include "utils/list.h"
#include "utils/misc.h"
#include "utils/str.h"

struct backend_plugins {
	UT_hash_handle hh;
	const char *backend_name;
	struct list_node plugins;
} *backend_plugins;

struct backend_plugin {
	const char *backend_name;
	picom_backend_plugin_entrypoint entrypoint;
	void *user_data;
	struct list_node siblings;
};

static bool add_backend_plugin(const char *backend_name, uint64_t major, uint64_t minor,
                               picom_backend_plugin_entrypoint entrypoint, void *user_data) {
	if (major != PICOM_BACKEND_MAJOR || minor > PICOM_BACKEND_MINOR) {
		log_error("Cannot add plugin for backend %s, because the requested "
		          "version %" PRIu64 ".%" PRIu64 " is incompatible with the our "
		          "%lu.%lu",
		          backend_name, major, minor, PICOM_BACKEND_MAJOR,
		          PICOM_BACKEND_MINOR);
		return false;
	}

	auto plugin = ccalloc(1, struct backend_plugin);
	plugin->backend_name = backend_name;
	plugin->entrypoint = entrypoint;
	plugin->user_data = user_data;

	struct backend_plugins *plugins = NULL;
	HASH_FIND_STR(backend_plugins, backend_name, plugins);
	if (!plugins) {
		plugins = ccalloc(1, struct backend_plugins);
		plugins->backend_name = strdup(backend_name);
		list_init_head(&plugins->plugins);
		HASH_ADD_STR(backend_plugins, backend_name, plugins);
	}
	list_insert_after(&plugins->plugins, &plugin->siblings);
	return true;
}

void api_backend_plugins_invoke(const char *backend_name, struct backend_base *backend) {
	struct backend_plugins *plugins = NULL;
	HASH_FIND_STR(backend_plugins, backend_name, plugins);
	if (!plugins) {
		return;
	}

	list_foreach(struct backend_plugin, plugin, &plugins->plugins, siblings) {
		plugin->entrypoint(backend, plugin->user_data);
	}
}

/// Variables added with `add_animation_variable`. Their names are kept separately, NULL
/// terminated, in the form `script_compile` takes. Compiled scripts refer to a variable
/// by its position here (`struct script_input::index`), so entries are never removed or
/// reordered.
static struct animation_variable {
	picom_animation_variable_provider provider;
	void *user_data;
} *animation_variables;
static const char **animation_variable_names;
/// Counts the frames picom prepares. Lives here with the other plugin state, so a reset
/// doesn't restart it and a number is never used twice.
static uint64_t frame_count;

/// Whether `name` is a letter, followed by letters, digits, '-' or '_'. The script parser
/// reads all of these as a name.
static bool is_animation_variable_name(const char *name) {
	if (!isalpha((unsigned char)name[0])) {
		return false;
	}
	for (const char *c = name + 1; *c != '\0'; c++) {
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') {
			return false;
		}
	}
	return true;
}

static bool
add_animation_variable(const char *plugin, const char *name,
                       picom_animation_variable_provider provider, void *user_data) {
	if (plugin == NULL || name == NULL) {
		log_error("Cannot add animation variable, because the plugin name or the "
		          "variable name is NULL.");
		return false;
	}
	if (provider == NULL) {
		log_error("Cannot add animation variable \"%s\" of plugin \"%s\", "
		          "because "
		          "the provider is NULL.",
		          name, plugin);
		return false;
	}
	// The plugin name keeps its variables apart from picom's own, now and later, and
	// from those of other plugins.
	if (!starts_with(plugin, "plugin-", false) ||
	    !is_animation_variable_name(plugin + strlen("plugin-"))) {
		log_error("Cannot add animation variable \"%s\" of plugin \"%s\", "
		          "because "
		          "a plugin name must be \"plugin-\", then a letter, followed by "
		          "letters, digits, '-' or '_'.",
		          name, plugin);
		return false;
	}
	if (!is_animation_variable_name(name)) {
		log_error("Cannot add animation variable \"%s\" of plugin \"%s\", "
		          "because "
		          "a variable name must be a letter followed by letters, digits, "
		          "'-' or '_'.",
		          name, plugin);
		return false;
	}
	if (animation_variables == NULL) {
		animation_variables = dynarr_new(struct animation_variable, 4);
		animation_variable_names = dynarr_new(const char *, 4);
		dynarr_push(animation_variable_names, NULL);
	}
	char *script_name = NULL;
	casprintf(&script_name, "%s-%s", plugin, name);
	for (unsigned i = 0; animation_variable_names[i] != NULL; i++) {
		if (strcmp(animation_variable_names[i], script_name) == 0) {
			log_error("Cannot add animation variable \"%s\", because it "
			          "already exists.",
			          script_name);
			free(script_name);
			return false;
		}
	}

	auto variable = (struct animation_variable){provider, user_data};
	dynarr_push(animation_variables, variable);
	// Keep the NULL at the end of the names
	dynarr_last(animation_variable_names) = script_name;
	dynarr_push(animation_variable_names, NULL);
	return true;
}

const char *const *api_animation_variable_names(void) {
	return animation_variable_names;
}

void api_frame_begin(void) {
	frame_count++;
}

void api_animation_variables_fill(struct script_instance *instance, const char *trigger,
                                  uint32_t window, uint32_t client) {
	const struct script_input *inputs = NULL;
	auto n_inputs = script_inputs(instance->script, &inputs);
	if (n_inputs == 0) {
		return;
	}
	struct picom_animation_start start = {
	    .trigger = trigger,
	    .window = window,
	    .client = client,
	    .frame = frame_count,
	};
	for (unsigned i = 0; i < n_inputs; i++) {
		// A copy, because a provider that adds a variable can move the array.
		auto variable = animation_variables[inputs[i].index];
		instance->memory[inputs[i].slot] =
		    variable.provider(&start, variable.user_data);
	}
}

static struct picom_api picom_api = {
    .add_backend_plugin = add_backend_plugin,
    .add_animation_variable = add_animation_variable,
};

PICOM_PUBLIC_API const struct picom_api *
picom_api_get_interfaces(uint64_t major, uint64_t minor, const char *context) {
	if (major != PICOM_API_MAJOR || minor > PICOM_API_MINOR) {
		log_error("Cannot provide API interfaces to %s, because the requested"
		          "version %" PRIu64 ".%" PRIu64 " is incompatible with our "
		          "%lu.%lu",
		          context, major, minor, PICOM_API_MAJOR, PICOM_API_MINOR);
		return NULL;
	}
	return &picom_api;
}

#ifdef UNIT_TEST
#include <libconfig.h>

static struct picom_animation_start test_start;
static double test_provider(const struct picom_animation_start *start, void *user_data) {
	test_start = *start;
	*(int *)user_data += 1;
	return 42;
}

TEST_CASE(animation_variables) {
	// The registry is process-wide and tests run once per process, so the names added
	// here stay. No other test compiles scripts against them.
	static const char *const bad_plugins[] = {
	    "",          "unittest",   "plugin-",          "plugin-1",
	    "plugin--a", "plugin-a.b", "plugin-has space",
	};
	static const char *const bad_names[] = {"", "1abc", "-abc", "a.b", "has space"};
	int calls_a = 0, calls_b = 0;
	for (size_t i = 0; i < ARR_SIZE(bad_plugins); i++) {
		TEST_TRUE(!add_animation_variable(bad_plugins[i], "a", test_provider, &calls_a));
	}
	for (size_t i = 0; i < ARR_SIZE(bad_names); i++) {
		TEST_TRUE(!add_animation_variable("plugin-unittest", bad_names[i],
		                                  test_provider, &calls_a));
	}
	TEST_TRUE(!add_animation_variable(NULL, "a", test_provider, &calls_a));
	TEST_TRUE(!add_animation_variable("plugin-unittest", NULL, test_provider, &calls_a));
	TEST_TRUE(!add_animation_variable("plugin-unittest", "a", NULL, &calls_a));
	TEST_TRUE(add_animation_variable("plugin-unittest", "a", test_provider, &calls_a));
	TEST_TRUE(!add_animation_variable("plugin-unittest", "a", test_provider, &calls_a));
	TEST_TRUE(add_animation_variable("plugin-unittest", "b_2", test_provider, &calls_b));

	auto names = api_animation_variable_names();
	unsigned n = 0;
	while (names[n] != NULL) {
		n++;
	}
	TEST_EQUAL(n, 2);
	TEST_STREQUAL(names[0], "plugin-unittest-a");
	TEST_STREQUAL(names[1], "plugin-unittest-b_2");

	// Filling a script that reads one of them calls that provider once, with the
	// start, and the other one not at all.
	config_t cfg;
	config_init(&cfg);
	TEST_TRUE(config_read_string(&cfg, "a = \"plugin-unittest-b_2 + 1\";"));
	struct script_output_info outputs[] = {{"a"}, {NULL}};
	char *err = NULL;
	auto script = script_compile(
	    config_root_setting(&cfg),
	    (struct script_parse_config){.output_info = outputs, .inputs = names}, &err);
	TEST_EQUAL(err, NULL);
	TEST_NOTEQUAL(script, NULL);
	if (script != NULL) {
		auto instance = script_instance_new(script);
		api_animation_variables_fill(instance, "open", 1, 2);
		TEST_EQUAL(calls_a, 0);
		TEST_EQUAL(calls_b, 1);
		TEST_STREQUAL(test_start.trigger, "open");
		TEST_EQUAL(test_start.window, 1);
		TEST_EQUAL(test_start.client, 2);
		TEST_EQUAL(script_instance_evaluate(instance, NULL, true), SCRIPT_EVAL_OK);
		TEST_EQUAL(instance->memory[outputs[0].slot], 43);
		free(instance);
		script_free(script);
	}
	free(err);
	config_destroy(&cfg);
}
#endif
