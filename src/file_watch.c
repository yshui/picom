#include <errno.h>
#include <string.h>
#include <sys/inotify.h>

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

static void file_watch_ev_cb(EV_P_ struct ev_io *w, int revent attr_unused) {
	auto fwr = (struct file_watch_registry *)w;
	struct inotify_event inotify_event;

	while (true) {
		auto ret = read(w->fd, &inotify_event, sizeof(struct inotify_event));
		if (ret < 0) {
			if (errno != EAGAIN) {
				log_error("Failed to read from inotify fd: %s",
				          strerror(errno));
			}
			break;
		}

		struct watched_file *wf = NULL;
		HASH_FIND_INT(fwr->reg, &inotify_event.wd, wf);
		if (!wf) {
			log_warn("Got notification for a file I didn't watch.");
			continue;
		}
		wf->cb(wf->ud);
	}
}

void *file_watch_init(EV_P) {
	log_debug("Starting watching for file changes");
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		log_error("inotify_init1 failed: %s", strerror(errno));
		return NULL;
	}
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
		free(i);
	}

	ev_io_stop(EV_A_ & fwr->w);
	close(fwr->w.fd);
	free(fwr);
}

bool file_watch_add(void *_fwr, const char *filename, file_watch_cb_t cb, void *ud) {
	log_debug("Adding \"%s\" to watched files", filename);
	auto fwr = (struct file_watch_registry *)_fwr;
	int wd = inotify_add_watch(fwr->w.fd, filename,
	                           IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
	if (wd < 0) {
		log_error("Failed to watch file \"%s\": %s", filename, strerror(errno));
		return false;
	}

	auto w = ccalloc(1, struct watched_file);
	w->wd = wd;
	w->cb = cb;
	w->ud = ud;

	HASH_ADD_INT(fwr->reg, wd, w);
	return true;
}
