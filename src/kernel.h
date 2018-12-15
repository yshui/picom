// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Code for generating convolution kernels

typedef struct conv {
  int size;
  double data[];
} conv;

/// Calculate the sum of a rectangle part of the convolution kernel
/// the rectangle is defined by top left (x, y), and a size (width x height)
double
sum_kernel(conv *map, int x, int y, int width, int height);

/// Create a kernel with gaussian distribution of radius r
conv *gaussian_kernel(double r);
