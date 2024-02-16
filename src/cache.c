#include <uthash.h>

#include "cache.h"

struct cache_handle *cache_get(struct cache *c, const char *key) {
	struct cache_handle *e;
	HASH_FIND_STR(c->entries, key, e);
	return e;
}

int cache_get_or_fetch(struct cache *c, const char *key, struct cache_handle **value,
                       void *user_data, cache_getter_t getter) {
	*value = cache_get(c, key);
	if (*value) {
		return 0;
	}

	int err = getter(c, key, value, user_data);
	assert(err <= 0);
	if (err < 0) {
		return err;
	}
	(*value)->key = strdup(key);

	HASH_ADD_STR(c->entries, key, *value);
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

void cache_invalidate(struct cache *c, const char *key, cache_free_t free_fn) {
	struct cache_handle *e;
	HASH_FIND_STR(c->entries, key, e);

	if (e) {
		cache_invalidate_impl(c, e, free_fn);
	}
}

void cache_invalidate_all(struct cache *c, cache_free_t free_fn) {
	struct cache_handle *e, *tmpe;
	HASH_ITER(hh, c->entries, e, tmpe) {
		cache_invalidate_impl(c, e, free_fn);
	}
}
