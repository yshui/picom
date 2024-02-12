#include <dlfcn.h>
#include <libunwind-x86_64.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>

static ssize_t (*orig_recvmsg)(int, struct msghdr *, int) = NULL;
thread_local char buffer[4096];

__attribute__((noinline)) void recvmsg_stack_trace_probe(const char *ptr, uint64_t size) {
	__asm__ volatile("" : : "r"(ptr), "r"(size));
}

void backtrace() {
	unw_cursor_t cursor;
	unw_context_t context;

	unw_getcontext(&context);
	unw_init_local(&cursor, &context);

	int n = 0;
	size_t buffer_offset = 0;
	while (unw_step(&cursor)) {
		unw_word_t ip, sp, off;

		unw_get_reg(&cursor, UNW_REG_IP, &ip);
		unw_get_reg(&cursor, UNW_REG_SP, &sp);

		char symbol[256] = {"<unknown>"};
		char *name = symbol;

		unw_get_proc_name(&cursor, symbol, sizeof(symbol), &off);

		size_t written = (size_t)snprintf(
		    buffer + buffer_offset, sizeof(buffer) - buffer_offset,
		    "#%-2d 0x%016" PRIxPTR " sp=0x%016" PRIxPTR " %s + 0x%" PRIxPTR "\n",
		    ++n, ip, sp, name, off);
		if (written >= sizeof(buffer) - buffer_offset) {
			break;
		}
		buffer_offset += written;
	}
	recvmsg_stack_trace_probe(buffer, buffer_offset);
}
ssize_t recvmsg(int socket, struct msghdr *message, int flags) {
	if (!orig_recvmsg) {
		orig_recvmsg = dlsym((void *)RTLD_NEXT, "recvmsg");
	}
	backtrace();
	return orig_recvmsg(socket, message, flags);
}
