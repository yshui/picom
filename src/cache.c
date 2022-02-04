#include <uthash.h>

#include "cache.h"
#include "compiler.h"
#include "utils.h"

struct cache_entry {
	char *key;
	void *value;
	UT_hash_handle hh;
};

struct cache {
	cache_getter_t getter;
	cache_free_t free;
	void *user_data;
	struct cache_entry *entries;
};

void cache_set(struct cache *c, const char *key, void *data) {
	struct cache_entry *e = NULL;
	HASH_FIND_STR(c->entries, key, e);
	CHECK(!e);

	e = ccalloc(1, struct cache_entry);
	e->key = strdup(key);
	e->value = data;
	HASH_ADD_STR(c->entries, key, e);
}

void *cache_get(struct cache *c, const char *key, int *err) {
	struct cache_entry *e;
	HASH_FIND_STR(c->entries, key, e);
	if (e) {
		return e->value;
	}

	int tmperr;
	if (!err) {
		err = &tmperr;
	}

	*err = 0;
	e = ccalloc(1, struct cache_entry);
	e->key = strdup(key);
	e->value = c->getter(c->user_data, key, err);
	if (*err) {
		free(e->key);
		free(e);
		return NULL;
	}

	HASH_ADD_STR(c->entries, key, e);
	return e->value;
}

static inline void _cache_invalidate(struct cache *c, struct cache_entry *e) {
	if (c->free) {
		c->free(c->user_data, e->value);
	}
	free(e->key);
	HASH_DEL(c->entries, e);
	free(e);
}

void cache_invalidate(struct cache *c, const char *key) {
	struct cache_entry *e;
	HASH_FIND_STR(c->entries, key, e);

	if (e) {
		_cache_invalidate(c, e);
	}
}

void cache_invalidate_all(struct cache *c) {
	struct cache_entry *e, *tmpe;
	HASH_ITER(hh, c->entries, e, tmpe) {
		_cache_invalidate(c, e);
	}
}

void *cache_free(struct cache *c) {
	void *ret = c->user_data;
	cache_invalidate_all(c);
	free(c);
	return ret;
}

struct cache *new_cache(void *ud, cache_getter_t getter, cache_free_t f) {
	auto c = ccalloc(1, struct cache);
	c->user_data = ud;
	c->getter = getter;
	c->free = f;
	return c;
}
