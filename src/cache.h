#pragma once

struct cache;

typedef void *(*cache_getter_t)(void *user_data, const char *key, int *err);
typedef void (*cache_free_t)(void *user_data, void *data);
struct cache *new_cache(void *user_data, cache_getter_t getter, cache_free_t f);

void *cache_get(struct cache *, const char *key, int *err);
void cache_invalidate(struct cache *, const char *key);
void cache_invalidate_all(struct cache *);

/// Returns the user data passed to `new_cache`
void *cache_free(struct cache *);
