
#include "c2.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "log.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	log_init_tls();
	if (size == 0) {
		return 0;
	}
	if (data[size - 1] != 0) {
		return 0;
	}
	c2_lptr_t *cond = c2_parse(NULL, (char *)data, NULL);
	if (!cond) {
		return 0;
	}

	// If we can parse it, we check if it roundtrips.
	// Except when the input or the stringified condition has ':' at the second
	// position, because that becomes a "legacy" pattern and is parsed differently.
	char *str = strdup(c2_lptr_to_str(cond));
	c2_free_lptr(cond, NULL);
	if (str[1] == ':') {
		free(str);
		return 0;
	}

	c2_lptr_t *cond2 = c2_parse(NULL, str, NULL);
	// The stringified condition could legitimately fail to parse, for example,
	// "a=1 || b=2 || c=3 || ... ", when stringified, will be "((((((a=1 || b=2) ||
	// c=3) || ...)", which will fail to parse because of the parenthese nest level
	// limit.
	if (cond2 == NULL) {
		free(str);
		return 0;
	}

	const char *str2 = c2_lptr_to_str(cond2);
	c2_free_lptr(cond2, NULL);
	if (strcmp(str, str2) != 0) {
		fprintf(stderr, "Mismatch: %s != %s\n", str, str2);
		abort();
	}

	free(str);
	return 0;
}
