
#include "c2.h"
#include <stddef.h>
#include <stdint.h>
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
	(void)cond;
	(void)size;
	return 0;        // Values other than 0 and -1 are reserved for future use.
}