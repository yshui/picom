#pragma once

#include "types.h"

typedef struct pixman_region32 region_t;
struct layout;
struct layout_manager;
struct backend_mask;
/// Remove unnecessary parts of the render commands.
///
/// After this call, the commands' regions of operations no longer point to their `mask`
/// fields. they point to `culled_mask` instead. The values of their `mask` fields are
/// retained, so later the commands can be "un-culled".
///
/// @param culled_mask use to stored the culled masks, must be have space to store at
///                    least `layout->number_of_commands` elements. They MUST NOT have
///                    initialized regions. This function will initialize their regions.
///                    These masks MUST NOT be freed until you call `commands_uncull`.
void commands_cull_with_damage(struct layout *layout, const region_t *damage,
                               struct geometry blur_size, struct backend_mask *culled_mask);

/// Un-do the effect of `commands_cull_with_damage`
void commands_uncull(struct layout *layout);

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
