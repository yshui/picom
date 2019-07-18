// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdio.h>
#include <xcb/xcb.h>

struct session;
struct backend_base;

/// A list of possible drivers.
/// The driver situation is a bit complicated. There are two drivers we care about: the
/// DDX, and the OpenGL driver. They are usually paired, but not always, since there is
/// also the generic modesetting driver.
/// This enum represents _both_ drivers.
enum driver {
	DRIVER_AMDGPU = 1, // AMDGPU for DDX, radeonsi for OpenGL
	DRIVER_RADEON = 2, // ATI for DDX, mesa r600 for OpenGL
	DRIVER_FGLRX = 4,
	DRIVER_NVIDIA = 8,
	DRIVER_NOUVEAU = 16,
	DRIVER_INTEL = 32,
	DRIVER_MODESETTING = 64,
};

/// Return a list of all drivers currently in use by the X server.
/// Note, this is a best-effort test, so no guarantee all drivers will be detected.
enum driver detect_driver(xcb_connection_t *, struct backend_base *, xcb_window_t);

// Print driver names to stdout, for diagnostics
static inline void print_drivers(enum driver drivers) {
	if (drivers & DRIVER_AMDGPU) {
		printf("AMDGPU, ");
	}
	if (drivers & DRIVER_RADEON) {
		printf("Radeon, ");
	}
	if (drivers & DRIVER_FGLRX) {
		printf("fglrx, ");
	}
	if (drivers & DRIVER_NVIDIA) {
		printf("NVIDIA, ");
	}
	if (drivers & DRIVER_NOUVEAU) {
		printf("nouveau, ");
	}
	if (drivers & DRIVER_INTEL) {
		printf("Intel, ");
	}
	if (drivers & DRIVER_MODESETTING) {
		printf("modesetting, ");
	}
	printf("\b\b \n");
}
