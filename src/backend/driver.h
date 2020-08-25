// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stddef.h>
#include <stdio.h>
#include <xcb/xcb.h>

#include "utils.h"

struct session;
struct backend_base;

// A list of known driver quirks:
// *  NVIDIA driver doesn't like seeing the same pixmap under different
//    ids, so avoid naming the pixmap again when it didn't actually change.

/// A list of possible drivers.
/// The driver situation is a bit complicated. There are two drivers we care about: the
/// DDX, and the OpenGL driver. They are usually paired, but not always, since there is
/// also the generic modesetting driver.
/// This enum represents _both_ drivers.
enum driver {
	DRIVER_AMDGPU = 1,        // AMDGPU for DDX, radeonsi for OpenGL
	DRIVER_RADEON = 2,        // ATI for DDX, mesa r600 for OpenGL
	DRIVER_FGLRX = 4,
	DRIVER_NVIDIA = 8,
	DRIVER_NOUVEAU = 16,
	DRIVER_INTEL = 32,
	DRIVER_MODESETTING = 64,
};

static const char *driver_names[] = {
    "AMDGPU", "Radeon", "fglrx", "NVIDIA", "nouveau", "Intel", "modesetting",
};

/// Return a list of all drivers currently in use by the X server.
/// Note, this is a best-effort test, so no guarantee all drivers will be detected.
enum driver detect_driver(xcb_connection_t *, struct backend_base *, xcb_window_t);

/// Apply driver specified global workarounds. It's safe to call this multiple times.
void apply_driver_workarounds(struct session *ps, enum driver);

// Print driver names to stdout, for diagnostics
static inline void print_drivers(enum driver drivers) {
	const char *seen_drivers[ARR_SIZE(driver_names)];
	int driver_count = 0;
	for (size_t i = 0; i < ARR_SIZE(driver_names); i++) {
		if (drivers & (1ul << i)) {
			seen_drivers[driver_count++] = driver_names[i];
		}
	}

	if (driver_count > 0) {
		printf("%s", seen_drivers[0]);
		for (int i = 1; i < driver_count; i++) {
			printf(", %s", seen_drivers[i]);
		}
	}
	printf("\n");
}
