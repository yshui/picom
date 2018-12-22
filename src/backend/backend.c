#include "backend.h"

backend_info_t *backend_list[NUM_BKEND] = {[BKEND_XRENDER] = &xrender_backend};

bool default_is_win_transparent(void *backend_data, win *w, void *win_data) {
	return w->mode != WMODE_SOLID;
}

bool default_is_frame_transparent(void *backend_data, win *w, void *win_data) {
	return w->frame_opacity != 1;
}
