// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <uthash.h>

#include "cache.h"
#include "misc.h"

struct cache_handle *cache_get(struct cache *c, const char *key, size_t keylen) {
	struct cache_handle *e;
	HASH_FIND(hh, c->entries, key, keylen, e);
	return e;
}

int cache_get_or_fetch(struct cache *c, const char *key, size_t keylen,
                       struct cache_handle **value, void *user_data, cache_getter_t getter) {
	*value = cache_get(c, key, keylen);
	if (*value) {
		return 0;
	}

	int err = getter(c, key, keylen, value, user_data);
	assert(err <= 0);
	if (err < 0) {
		return err;
	}
	// Add a NUL terminator to make things easier
	(*value)->key = ccalloc(keylen + 1, char);
	memcpy((*value)->key, key, keylen);

	HASH_ADD_KEYPTR(hh, c->entries, (*value)->key, keylen, *value);
	return 1;
}

static inline void
cache_invalidate_impl(struct cache *c, struct cache_handle *e, cache_free_t free_fn) {
	free(e->key);
	HASH_DEL(c->entries, e);
	if (free_fn) {
		free_fn(c, e);
	}
}

void cache_invalidate_all(struct cache *c, cache_free_t free_fn) {
	struct cache_handle *e, *tmpe;
	HASH_ITER(hh, c->entries, e, tmpe) {
		cache_invalidate_impl(c, e, free_fn);
	}
}
