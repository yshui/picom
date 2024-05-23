// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdint.h>

#define PICOM_API_MAJOR (0UL)
#define PICOM_API_MINOR (1UL)

struct picom_api {};

const struct picom_api *
picom_api_get_interfaces(uint64_t major, uint64_t minor, const char *context);
