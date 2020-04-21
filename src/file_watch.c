#include <errno.h>
#include <string.h>
#ifdef HAS_INOTIFY
#include <sys/inotify.h>
#elif HAS_KQUEUE
// clang-format off
#include <sys/types.h>
// clang-format on
#include <sys/event.h>
#undef EV_ERROR              // Avoid clashing with libev's EV_ERROR
#include <fcntl.h>           // For O_RDONLY
#include <sys/time.h>        // For struct timespec
#include <unistd.h>          // For open
#endif

#include <ev.h>
#include <uthash.h>

#include "file_watch.h"
#include "list.h"
#include "log.h"
#include "utils.h"

struct watched_file {
	int wd;
	void *ud;
	file_watch_cb_t cb;

	UT_hash_handle hh;
};

struct file_watch_registry {
	struct ev_io w;

	struct watched_file *reg;
};

static void file_watch_ev_cb(EV_P attr_unused, struct ev_io *w, int revent attr_unused) {
	auto fwr = (struct file_watch_registry *)w;

	while (true) {
		int wd = -1;
#ifdef HAS_INOTIFY
		struct inotify_event inotify_event;
		auto ret = read(w->fd, &inotify_event, sizeof(struct inotify_event));
		if (ret < 0) {
			if (errno != EAGAIN) {
				log_error_errno("Failed to read from inotify fd");
			}
			break;
		}
		wd = inotify_event.wd;
#elif HAS_KQUEUE
		struct kevent ev;
		struct timespec timeout = {0};
		int ret = kevent(fwr->w.fd, NULL, 0, &ev, 1, &timeout);
		if (ret <= 0) {
			if (ret < 0) {
				log_error_errno("Failed to get kevent");
			}
			break;
		}
		wd = (int)ev.ident;
#else
		assert(false);
#endif

		struct watched_file *wf = NULL;
		HASH_FIND_INT(fwr->reg, &wd, wf);
		if (!wf) {
			log_warn("Got notification for a file I didn't watch.");
			continue;
		}
		wf->cb(wf->ud);
	}
}

void *file_watch_init(EV_P) {
	log_debug("Starting watching for file changes");
	int fd = -1;
#ifdef HAS_INOTIFY
	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		log_error_errno("inotify_init1 failed");
		return NULL;
	}
#elif HAS_KQUEUE
	fd = kqueue();
	if (fd < 0) {
		log_error_errno("Failed to create kqueue");
		return NULL;
	}
#else
	log_info("No file watching support found on the host system.");
	return NULL;
#endif
	auto fwr = ccalloc(1, struct file_watch_registry);
	ev_io_init(&fwr->w, file_watch_ev_cb, fd, EV_READ);
	ev_io_start(EV_A_ & fwr->w);

	return fwr;
}

void file_watch_destroy(EV_P_ void *_fwr) {
	log_debug("Stopping watching for file changes");
	auto fwr = (struct file_watch_registry *)_fwr;
	struct watched_file *i, *tmp;

	HASH_ITER(hh, fwr->reg, i, tmp) {
		HASH_DEL(fwr->reg, i);
#ifdef HAS_KQUEUE
		// kqueue watch descriptors are file descriptors of
		// the files we are watching, so we need to close
		// them
		close(i->wd);
#endif
		free(i);
	}

	ev_io_stop(EV_A_ & fwr->w);
	close(fwr->w.fd);
	free(fwr);
}

bool file_watch_add(void *_fwr, const char *filename, file_watch_cb_t cb, void *ud) {
	log_debug("Adding \"%s\" to watched files", filename);
	auto fwr = (struct file_watch_registry *)_fwr;
	int wd = -1;

	struct stat statbuf;
	int ret = stat(filename, &statbuf);
	if (ret < 0) {
		log_error_errno("Failed to retrieve information about file \"%s\"", filename);
		return false;
	}
	if (!S_ISREG(statbuf.st_mode)) {
		log_info("\"%s\" is not a regular file, not watching it.", filename);
		return false;
	}

#ifdef HAS_INOTIFY
	wd = inotify_add_watch(fwr->w.fd, filename,
	                       IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
	if (wd < 0) {
		log_error_errno("Failed to watch file \"%s\"", filename);
		return false;
	}
#elif HAS_KQUEUE
	wd = open(filename, O_RDONLY);
	if (wd < 0) {
		log_error_errno("Cannot open file \"%s\" for watching", filename);
		return false;
	}

	uint32_t fflags = NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE | NOTE_ATTRIB;
	// NOTE_CLOSE_WRITE is relatively new, so we cannot just use it
#ifdef NOTE_CLOSE_WRITE
	fflags |= NOTE_CLOSE_WRITE;
#else
	// NOTE_WRITE will receive notification more frequent than necessary, so is less
	// preferrable
	fflags |= NOTE_WRITE;
#endif
	struct kevent ev = {
	    .ident = (unsigned int)wd,        // the wd < 0 case is checked above
	    .filter = EVFILT_VNODE,
	    .flags = EV_ADD | EV_CLEAR,
	    .fflags = fflags,
	    .data = 0,
	    .udata = NULL,
	};
	if (kevent(fwr->w.fd, &ev, 1, NULL, 0, NULL) < 0) {
		log_error_errno("Failed to register kevent");
		close(wd);
		return false;
	}
#else
	assert(false);
#endif        // HAS_KQUEUE

	auto w = ccalloc(1, struct watched_file);
	w->wd = wd;
	w->cb = cb;
	w->ud = ud;

	HASH_ADD_INT(fwr->reg, wd, w);
	return true;
}
