#pragma once

#include "types.h"

typedef struct pixman_region32 region_t;
struct layout;
struct layout_manager;

/// Calculate damage of the screen for the last `buffer_age` layouts. Assuming the
/// current, yet to be rendered frame is numbered frame 0, the previous frame is numbered
/// frame -1, and so on. This function returns the region of the screen that will be
/// different between frame `-buffer_age` and frame 0. The region is in screen
/// coordinates. `buffer_age` is at least 1, and must be less than the `max_buffer_age`
/// passed to the `layout_manager_new` that was used to create `lm`.
///
/// The layouts you want to calculate damage for must already have commands built for
/// them. `blur_size` is the size of the background blur, and is assumed to not change
/// over time.
///
/// Note `layout_manager_damage` cannot take desktop background change into
/// account.
void layout_manager_damage(struct layout_manager *lm, unsigned buffer_age,
                           struct geometry blur_size, region_t *damage);
