// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Macro metaprogramming

#define _APPLY1(a, ...) a(__VA_ARGS__)
#define _APPLY2(a, ...) a(__VA_ARGS__)
#define _APPLY3(a, ...) a(__VA_ARGS__)
#define _APPLY4(a, ...) a(__VA_ARGS__)

#define RIOTA1(x) x
#define RIOTA2(x) RIOTA1(x##1), RIOTA1(x##0)
#define RIOTA4(x) RIOTA2(x##1), RIOTA2(x##0)
#define RIOTA8(x) RIOTA4(x##1), RIOTA4(x##0)
#define RIOTA16(x) RIOTA8(x##1), RIOTA8(x##0)
/// Generate a list containing 31, 30, ..., 0, in binary
#define RIOTA32(x) RIOTA16(x##1), RIOTA16(x##0)

#define CONCAT2(a, b) a##b
#define CONCAT1(a, b) CONCAT2(a, b)
#define CONCAT(a, b) CONCAT1(a, b)

#define _ARGS_HEAD(head, ...) head
#define _ARGS_SKIP4(_1, _2, _3, _4, ...) __VA_ARGS__
#define _ARGS_SKIP8(...) _APPLY1(_ARGS_SKIP4, _ARGS_SKIP4(__VA_ARGS__))
#define _ARGS_SKIP16(...) _APPLY2(_ARGS_SKIP8, _ARGS_SKIP8(__VA_ARGS__))
#define _ARGS_SKIP32(...) _APPLY3(_ARGS_SKIP16, _ARGS_SKIP16(__VA_ARGS__))

/// Return the 33rd argument
#define _ARG33(...) _APPLY4(_ARGS_HEAD, _ARGS_SKIP32(__VA_ARGS__))

/// Return the number of arguments passed in binary, handles at most 31 elements
#define VA_ARGS_LENGTH(...) _ARG33(0, ##__VA_ARGS__, RIOTA32(0))

#define LIST_APPLY_000000(fn, sep, ...)
#define LIST_APPLY_000001(fn, sep, x, ...) fn(x)
#define LIST_APPLY_000010(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000001(fn, sep, __VA_ARGS__)
#define LIST_APPLY_000011(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000010(fn, sep, __VA_ARGS__)
#define LIST_APPLY_000100(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000011(fn, sep, __VA_ARGS__)
#define LIST_APPLY_000101(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000100(fn, sep, __VA_ARGS__)
#define LIST_APPLY_000110(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000101(fn, sep, __VA_ARGS__)
#define LIST_APPLY_000111(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000110(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001000(fn, sep, x, ...) fn(x) sep() LIST_APPLY_000111(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001001(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001000(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001010(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001001(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001011(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001010(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001100(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001011(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001101(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001100(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001110(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001101(fn, sep, __VA_ARGS__)
#define LIST_APPLY_001111(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001110(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010000(fn, sep, x, ...) fn(x) sep() LIST_APPLY_001111(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010001(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010000(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010010(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010001(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010011(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010010(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010100(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010011(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010101(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010100(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010110(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010101(fn, sep, __VA_ARGS__)
#define LIST_APPLY_010111(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010110(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011000(fn, sep, x, ...) fn(x) sep() LIST_APPLY_010111(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011001(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011000(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011010(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011001(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011011(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011010(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011100(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011011(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011101(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011100(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011110(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011101(fn, sep, __VA_ARGS__)
#define LIST_APPLY_011111(fn, sep, x, ...) fn(x) sep() LIST_APPLY_011110(fn, sep, __VA_ARGS__)
#define LIST_APPLY_(N, fn, sep, ...) CONCAT(LIST_APPLY_, N)(fn, sep, __VA_ARGS__)
#define LIST_APPLY(fn, sep, ...)                                                         \
	LIST_APPLY_(VA_ARGS_LENGTH(__VA_ARGS__), fn, sep, __VA_ARGS__)

#define SEP_COMMA() ,
#define SEP_COLON() ;
#define SEP_NONE()
