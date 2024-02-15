#pragma once

struct cache;

typedef void *(*cache_getter_t)(void *user_data, const char *key, int *err);
typedef void (*cache_free_t)(void *user_data, void *data);

/// Create a cache with `getter`, and a free function `f` which is used to free the cache
/// value when they are invalidated.
///
/// `user_data` will be passed to `getter` and `f` when they are called.
struct cache *new_cache(void *user_data, cache_getter_t getter, cache_free_t f);

/// Get a value from the cache. If the value doesn't present in the cache yet, the
/// getter will be called, and the returned value will be stored into the cache.
void *cache_get_or_fetch(struct cache *, const char *key, int *err);

/// Get a value from the cache. If the value doesn't present in the cache, NULL will be
/// returned.
void *cache_get(struct cache *, const char *key);

/// Invalidate a value in the cache.
void cache_invalidate(struct cache *, const char *key);

/// Invalidate all values in the cache.
void cache_invalidate_all(struct cache *);

/// Invalidate all values in the cache and free it. Returns the user data passed to
/// `new_cache`
void *cache_free(struct cache *);
