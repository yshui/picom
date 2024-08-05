#include <libconfig.h>
#include <uthash.h>
#include "compiler.h"        // IWYU pragma: keep
#include "transition/script.h"
#include "transition/script_internal.h"
#include "utils/dynarr.h"
#include "wm/win.h"

enum knob_type {
	KNOB_NUMBER,
	KNOB_CHOICE,
};

struct knob {
	UT_hash_handle hh;
	const char *name;
	enum knob_type type;

	union {
		struct {
			double default_value;
		} number;
		struct {
			char **choices;
			unsigned n_choices;
			unsigned default_choice;
		};
	};
	bool emitted;
};

struct placeholder {
	struct knob *source;
	double *value_for_choices;
};

bool config_extra_get_float(config_setting_t *setting, double *value) {
	if (config_setting_type(setting) != CONFIG_TYPE_FLOAT &&
	    config_setting_type(setting) != CONFIG_TYPE_INT &&
	    config_setting_type(setting) != CONFIG_TYPE_INT64) {
		return false;
	}
	*value = config_setting_get_float(setting);
	return true;
}

bool config_extra_get_int(config_setting_t *setting, int *value) {
	if (config_setting_type(setting) != CONFIG_TYPE_INT &&
	    config_setting_type(setting) != CONFIG_TYPE_INT64) {
		return false;
	}
	*value = config_setting_get_int(setting);
	return true;
}

char *sanitized_name(const char *name) {
	char *ret = strdup(name);
	for (char *p = ret; *p; p++) {
		if (*p == '-') {
			*p = '_';
		}
	}
	return ret;
}

void free_charp(void *p) {
	free(*(char **)p);
}

#define scopedp(type) cleanup(free_##type##p) type *

#define MAX_PLACEHOLDERS 10

void codegen(const char *name, const char *body, const struct placeholder *placeholders) {
	auto ident = sanitized_name(name);
	printf("static struct script *script_template__%s(int *output_slots)\n%s\n",
	       ident, body);
	printf("static bool win_script_preset__%s(struct win_script *output, "
	       "config_setting_t *setting) {\n",
	       ident);
	printf("    output->script = script_template__%s(output->output_indices);\n", ident);
	for (size_t i = 0; i < MAX_PLACEHOLDERS; i++) {
		if (placeholders[i].source == NULL || placeholders[i].source->emitted) {
			continue;
		}

		auto knob = placeholders[i].source;
		scopedp(char) knob_ident = sanitized_name(knob->name);
		knob->emitted = true;
		if (knob->type == KNOB_NUMBER) {
			printf("    double knob_%s = %a;\n", knob_ident,
			       knob->number.default_value);
			printf("    config_setting_lookup_float(setting, \"%s\", "
			       "&knob_%s);\n",
			       knob->name, knob_ident);
			continue;
		}
		printf("    const char *knob_%s = \"%s\";\n", knob_ident,
		       knob->choices[knob->default_choice]);
		printf("    config_setting_lookup_string(setting, \"%s\", &knob_%s);\n",
		       knob->name, knob_ident);
		for (unsigned j = 0; j < MAX_PLACEHOLDERS; j++) {
			if (placeholders[j].source != knob) {
				continue;
			}
			printf("    double placeholder%u_%s;\n", j, knob_ident);
		}
		for (unsigned j = 0; j < knob->n_choices; j++) {
			printf("    if (strcmp(knob_%s, \"%s\") == 0) {\n", knob_ident,
			       knob->choices[j]);
			for (unsigned k = 0; k < MAX_PLACEHOLDERS; k++) {
				if (placeholders[k].source != knob) {
					continue;
				}
				printf("        placeholder%u_%s = %a;\n", k, knob_ident,
				       placeholders[k].value_for_choices[j]);
			}
			printf("    } else ");
		}
		printf("{\n");
		printf("        log_error(\"Invalid choice \\\"%%s\\\" for "
		       "option \\\"%s\\\". Line %%d.\", knob_%s, "
		       "config_setting_source_line(config_setting_get_member(setting, "
		       "\"%s\")));\n",
		       knob->name, knob_ident, knob->name);
		printf("        log_error(\"    Valid ones are: ");
		for (unsigned j = 0; j < knob->n_choices; j++) {
			printf("%s\\\"%s\\\"", j ? ", " : "", knob->choices[j]);
		}
		printf("\");\n");
		printf("        script_free(output->script);\n");
		printf("        output->script = NULL;\n");
		printf("        return false;\n");
		printf("    }\n");
	}
	printf("    struct script_specialization_context spec[] = {\n");
	for (size_t i = 0; i < 10; i++) {
		if (placeholders[i].source == NULL) {
			continue;
		}
		auto knob = placeholders[i].source;
		auto knob_ident = sanitized_name(knob->name);
		if (knob->type == KNOB_NUMBER) {
			printf("        {.offset = SCRIPT_CTX_PLACEHOLDER_BASE + %zu, "
			       ".value = knob_%s},\n",
			       i * 4, knob_ident);
		} else {
			printf("        {.offset = SCRIPT_CTX_PLACEHOLDER_BASE + %zu, "
			       ".value = placeholder%zu_%s},\n",
			       i * 4, i, knob_ident);
		}
		free(knob_ident);
	}
	printf("    };\n");
	printf("    script_specialize(output->script, spec, ARR_SIZE(spec));\n");
	printf("    return true;\n");
	printf("}\n");
	free(ident);
}

/// Syntax for defining knobs and placeholders:
///
/// {
///     # other settings...
///     # ....
///
///     __knobs = {
///         knob1 = 0.5; # knob1 is a number, default value 0.5
///
///         # knob2 is a choice, default choice is "default_choice" (index 2)
///         #        ┌----- index of the default choice
///         #        v
///         knob2 = (2, ["choice1", "choice2", "default_choice"]);
///     };
///     __placeholders = (
///         #┌----- index of the placeholder
///         #v
///         (1, "knob1"), # placeholder1 takes value from knob1
///
///         # placeholder2 takes value from knob2. Because knob2 is a choice,
///         # we need to provide a mapping from choice to value.
///         (2, "knob2", [1, 2, 0]);
///     );
/// }

static bool parse_knobs(const char *preset_name, config_setting_t *knob_settings,
                        config_setting_t *placeholder_settings, struct knob *knobs,
                        struct placeholder *placeholders) {
	struct knob *knobs_by_name = NULL;
	if (config_setting_length(knob_settings) > MAX_PLACEHOLDERS) {
		fprintf(stderr, "Too many knobs in %s, max %d allowed\n", preset_name,
		        MAX_PLACEHOLDERS);
		return false;
	}
	if (config_setting_length(placeholder_settings) > MAX_PLACEHOLDERS) {
		fprintf(stderr, "Too many placeholders in %s, max %d allowed\n",
		        preset_name, MAX_PLACEHOLDERS);
		return false;
	}
	unsigned n_knobs = 0;
	for (unsigned i = 0; i < (unsigned)config_setting_length(knob_settings); i++) {
		auto config = config_setting_get_elem(knob_settings, i);
		const char *name = config_setting_name(config);
		double default_value;
		auto knob = &knobs[n_knobs++];
		knob->name = strdup(name);
		if (config_extra_get_float(config, &default_value)) {
			knob->type = KNOB_NUMBER;
			knob->number.default_value = default_value;
			HASH_ADD_STR(knobs_by_name, name, knob);
			n_knobs++;
			continue;
		}
		if (!config_setting_is_list(config) || config_setting_length(config) != 2) {
			fprintf(stderr,
			        "Invalid placeholder %s in %s, line %d. It must be a "
			        "list of length 2.\n",
			        name, preset_name, config_setting_source_line(config));
			continue;
		}

		int default_choice;
		config_setting_t *choices = config_setting_get_elem(config, 1);
		if (!config_extra_get_int(config_setting_get_elem(config, 0), &default_choice) ||
		    choices == NULL || !config_setting_is_array(choices)) {
			fprintf(stderr,
			        "Invalid placeholder %s in %s, line %d. Failed to get "
			        "elements.\n",
			        name, preset_name, config_setting_source_line(config));
			continue;
		}

		auto n_choices = (unsigned)config_setting_length(choices);
		if (default_choice < 0 || (unsigned)default_choice >= n_choices) {
			fprintf(stderr,
			        "Invalid knob choice in %s, knob %s line %d. Default "
			        "choice out of range.\n",
			        preset_name, name, config_setting_source_line(config));
			continue;
		}
		knob->type = KNOB_CHOICE;
		knob->n_choices = 0;
		knob->choices = malloc(n_choices * sizeof(char *));
		knob->default_choice = (unsigned)default_choice;

		bool has_error = false;
		for (unsigned j = 0; j < n_choices; j++) {
			auto choice =
			    config_setting_get_string(config_setting_get_elem(choices, j));
			if (choice == NULL) {
				fprintf(stderr,
				        "Invalid knob choice in %s, knob %s line %d. "
				        "Failed to get choice.\n",
				        preset_name, name,
				        config_setting_source_line(config));
				has_error = true;
				break;
			}
			for (unsigned k = 0; k < j; k++) {
				if (strcmp(knob->choices[k], choice) == 0) {
					fprintf(stderr,
					        "Invalid knob choice in %s, knob %s line "
					        "%d. Duplicate choice %s.\n",
					        preset_name, name,
					        config_setting_source_line(config), choice);
					has_error = true;
					break;
				}
			}
			if (has_error) {
				break;
			}
			knob->choices[knob->n_choices++] = strdup(choice);
		}
		if (has_error) {
			for (unsigned j = 0; j < knob->n_choices; j++) {
				free(knob->choices[j]);
			}
			free(knob->choices);
			free((void *)knob->name);
			knob->choices = NULL;
			knob->name = NULL;
			continue;
		}
		HASH_ADD_STR(knobs_by_name, name, knob);
		n_knobs++;
	}

	for (unsigned i = 0; i < (unsigned)config_setting_length(placeholder_settings); i++) {
		auto config = config_setting_get_elem(placeholder_settings, i);
		if (!config_setting_is_list(config) || config_setting_length(config) < 2) {
			fprintf(stderr,
			        "Invalid placeholder in preset %s, line %d. Must be a "
			        "non-empty list.\n",
			        preset_name, config_setting_source_line(config));
			continue;
		}

		int index;
		if (!config_extra_get_int(config_setting_get_elem(config, 0), &index)) {
			fprintf(stderr,
			        "Invalid placeholder in preset %s, line %d. Index must "
			        "be an integer.\n",
			        preset_name, config_setting_source_line(config));
			continue;
		}

		auto placeholder = &placeholders[index];
		if (placeholder->source) {
			fprintf(stderr,
			        "Invalid placeholder in preset %s, line %d. Placeholder "
			        "with index %d already defined.\n",
			        preset_name, config_setting_source_line(config), index);
			continue;
		}
		BUG_ON(placeholder->value_for_choices != NULL);
		const char *source =
		    config_setting_get_string(config_setting_get_elem(config, 1));
		struct knob *knob;
		HASH_FIND_STR(knobs_by_name, source, knob);
		if (!knob) {
			fprintf(stderr,
			        "Invalid placeholder%d definition in %s, line "
			        "%d. Source knob %s not found.\n",
			        index, preset_name, config_setting_source_line(config),
			        source);
			continue;
		}

		if (config_setting_length(config) == 2) {
			if (knob->type != KNOB_NUMBER) {
				fprintf(stderr,
				        "Invalid placeholder%d definition in %s, line "
				        "%d. Source knob %s is not a number.\n",
				        index, preset_name,
				        config_setting_source_line(config), source);
				continue;
			}
			placeholder->source = knob;
		} else if (config_setting_length(config) == 3) {
			config_setting_t *value_for_choices =
			    config_setting_get_elem(config, 2);
			if (value_for_choices == NULL ||
			    !config_setting_is_array(value_for_choices)) {
				fprintf(stderr,
				        "Invalid placeholder%d definition in %s, line "
				        "%d. Failed to get elements.\n",
				        index, preset_name,
				        config_setting_source_line(config));
				continue;
			}
			if (knob->type != KNOB_CHOICE) {
				fprintf(stderr,
				        "Invalid placeholder%d definition in %s, line "
				        "%d. Source knob %s is not a choice.\n",
				        index, preset_name,
				        config_setting_source_line(config), source);
				continue;
			}
			if (knob->n_choices !=
			    (unsigned)config_setting_length(value_for_choices)) {
				fprintf(stderr,
				        "Invalid placeholder%d definition in %s, line "
				        "%d. Number of choices doesn't match.\n",
				        index, preset_name,
				        config_setting_source_line(config));
				continue;
			}
			placeholder->value_for_choices =
			    malloc(sizeof(double) * knob->n_choices);
			for (unsigned j = 0; j < knob->n_choices; j++) {
				double value;
				if (!config_extra_get_float(
				        config_setting_get_elem(value_for_choices, j), &value)) {
					fprintf(stderr,
					        "Invalid placeholder%d definition in %s, "
					        "line %d. Failed to get value.\n",
					        index, preset_name,
					        config_setting_source_line(config));
					free(placeholder->value_for_choices);
					placeholder->value_for_choices = NULL;
					break;
				}
				placeholder->value_for_choices[j] = value;
			}
			if (placeholder->value_for_choices == NULL) {
				continue;
			}
			placeholder->source = knob;
		} else {
			fprintf(stderr,
			        "Invalid placeholder%d definition in %s, line %d. "
			        "Excessive elements.\n",
			        index, preset_name, config_setting_source_line(config));
			continue;
		}
	}
	struct knob *k, *nk;
	HASH_ITER(hh, knobs_by_name, k, nk) {
		HASH_DEL(knobs_by_name, k);
	}
	return true;
}

int main(int argc, const char **argv) {
	if (argc != 2) {
		return 1;
	}

	log_init_tls();

	char **presets = dynarr_new(char *, 10);

	config_t cfg;
	config_init(&cfg);
	config_set_auto_convert(&cfg, 1);

	if (!config_read_file(&cfg, argv[1])) {
		fprintf(stderr, "Failed to read config file %s: %s\n", argv[1],
		        config_error_text(&cfg));
		config_destroy(&cfg);
		return 1;
	}

	auto settings = config_root_setting(&cfg);

	// win_script_context_info and 10 extra placeholder contexts, for
	// script_specialize()
	static const ptrdiff_t base = SCRIPT_CTX_PLACEHOLDER_BASE;
	struct script_context_info context_info[ARR_SIZE(win_script_context_info) + MAX_PLACEHOLDERS] = {
	    {"placeholder0", base},      {"placeholder1", base + 4},
	    {"placeholder2", base + 8},  {"placeholder3", base + 12},
	    {"placeholder4", base + 16}, {"placeholder5", base + 20},
	    {"placeholder6", base + 24}, {"placeholder7", base + 28},
	    {"placeholder8", base + 32}, {"placeholder9", base + 36},
	};
	memcpy(context_info + 10, win_script_context_info, sizeof(win_script_context_info));

	struct script_output_info outputs[ARR_SIZE(win_script_outputs)];
	memcpy(outputs, win_script_outputs, sizeof(win_script_outputs));

	struct script_parse_config parse_config = {
	    .context_info = context_info,
	    .output_info = NULL,
	};
	printf("// This file is generated by tools/animgen.c from %s\n", argv[1]);
	printf("// This file is included in git repository for "
	       "convenience only.\n");
	printf("// DO NOT EDIT THIS FILE!\n\n");

	printf("#include <libconfig.h>\n");
	printf("#include \"../script.h\"\n");
	printf("#include \"../curve.h\"\n");
	printf("#include \"../script_internal.h\"\n");
	printf("#include \"utils/misc.h\"\n");
	printf("#include \"config.h\"\n");
	for (unsigned i = 0; i < (unsigned)config_setting_length(settings); i++) {
		auto sub = config_setting_get_elem(settings, i);
		auto name = config_setting_name(sub);
		struct knob knobs[MAX_PLACEHOLDERS] = {};
		struct placeholder placeholders[MAX_PLACEHOLDERS] = {};

		auto knob_settings = config_setting_get_member(sub, "*knobs");
		if (knob_settings) {
			auto placeholder_settings =
			    config_setting_get_member(sub, "*placeholders");
			BUG_ON(!placeholder_settings);
			parse_knobs(name, knob_settings, placeholder_settings, knobs,
			            placeholders);
			config_setting_remove(sub, "*knobs");
			config_setting_remove(sub, "*placeholders");
			knob_settings = NULL;
		}

		char *err = NULL;
		auto script = script_compile(sub, parse_config, &err);
		if (!script) {
			fprintf(stderr, "Failed to compile script %s: %s\n", name, err);
			free(err);
			continue;
		}
		bool has_err = false;
		for (size_t j = 0; j < script->len; j++) {
			if (script->instrs[j].type != INST_LOAD_CTX) {
				continue;
			}
			if (script->instrs[j].ctx < base) {
				continue;
			}
			size_t index = (size_t)(script->instrs[j].ctx - base) / 4;
			BUG_ON(index >= ARR_SIZE(knobs));
			if (placeholders[index].source == NULL) {
				fprintf(stderr, "Placeholder %zu used, but not defined\n",
				        index);
				has_err = true;
				break;
			}
		}

		if (!has_err) {
			char *code = script_to_c(script, outputs);
			codegen(name, code, placeholders);
			free(code);

			dynarr_push(presets, strdup(name));
		}
		for (size_t j = 0; j < MAX_PLACEHOLDERS; j++) {
			if (placeholders[j].value_for_choices) {
				free(placeholders[j].value_for_choices);
			}
		}
		for (size_t j = 0; j < MAX_PLACEHOLDERS; j++) {
			if (knobs[j].type == KNOB_CHOICE) {
				for (unsigned k = 0; k < knobs[j].n_choices; k++) {
					free(knobs[j].choices[k]);
				}
				free(knobs[j].choices);
			}
			free((void *)knobs[j].name);
		}
		script_free(script);
	}

	config_destroy(&cfg);

	printf("struct {\n"
	       "    const char *name;\n"
	       "    bool (*func)(struct win_script *output, "
	       "config_setting_t *setting);\n"
	       "} win_script_presets[] = {\n");
	dynarr_foreach(presets, p) {
		auto ident = sanitized_name(*p);
		printf("    {\"%s\", win_script_preset__%s},\n", *p, ident);
		free(*p);
		free(ident);
	}
	printf("    {NULL, NULL},\n};\n");
	dynarr_free_pod(presets);
	return 0;
}
