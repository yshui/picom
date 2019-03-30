#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "compiler.h"
#include "string_utils.h"
#include "utils.h"

/// Report allocation failure without allocating memory
void report_allocation_failure(const char *func, const char *file, unsigned int line) {
	// Since memory allocation failed, we try to print this error message without any
	// memory allocation. Since logging framework allocates memory (and might even
	// have not been initialized yet), so we can't use it.
	char buf[11];
	int llen = uitostr(line, buf);
	const char msg1[] = " has failed to allocate memory, ";
	const char msg2[] = ". Aborting...\n";
	const struct iovec v[] = {
	    {.iov_base = (void *)func, .iov_len = strlen(func)},
	    {.iov_base = "()", .iov_len = 2},
	    {.iov_base = (void *)msg1, .iov_len = sizeof(msg1) - 1},
	    {.iov_base = "at ", .iov_len = 3},
	    {.iov_base = (void *)file, .iov_len = strlen(file)},
	    {.iov_base = ":", .iov_len = 1},
	    {.iov_base = buf, .iov_len = (size_t)llen},
	    {.iov_base = (void *)msg2, .iov_len = sizeof(msg2) - 1},
	};

	writev(STDERR_FILENO, v, ARR_SIZE(v));
	abort();

	unreachable;
}

// vim: set noet sw=8 ts=8 :
