#pragma once
#include <stdbool.h>

#include <ev.h>

typedef void (*file_watch_cb_t)(void *);

void *file_watch_init(EV_P);
bool file_watch_add(void *, const char *, file_watch_cb_t, void *);
void file_watch_destroy(EV_P_ void *);
