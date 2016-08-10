#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# vim:fileencoding=utf-8

import math
import argparse


class CGError(Exception):
    '''An error in the convolution kernel generator.'''
    def __init__(self, desc):
        super().__init__(desc)


class CGBadArg(CGError):
    '''An exception indicating an invalid argument has been passed to the
    convolution kernel generator.'''
    pass


def mbuild(width, height):
    """Build a NxN matrix filled with 0."""
    result = list()
    for i in range(height):
        result.append(list())
        for j in range(width):
            result[i].append(0.0)
    return result


def mdump(matrix):
    """Dump a matrix in natural format."""
    for col in matrix:
        print("[ ", end='')
        for ele in col:
            print(format(ele, "13.6g") + ", ", end=" ")
        print("],")


def mdumpcompton(matrix):
    """Dump a matrix in compton's format."""
    width = len(matrix[0])
    height = len(matrix)
    print("{},{},".format(width, height), end='')
    for i in range(height):
        for j in range(width):
            if int(height / 2) == i and int(width / 2) == j:
                continue
            print(format(matrix[i][j], ".6f"), end=",")
    print()


def mnormalize(matrix):
    """Scale a matrix according to the value in the center."""
    width = len(matrix[0])
    height = len(matrix)
    factor = 1.0 / matrix[int(height / 2)][int(width / 2)]
    if 1.0 == factor:
        return matrix
    for i in range(height):
        for j in range(width):
            matrix[i][j] *= factor
    return matrix


def mmirror4(matrix):
    """Do a 4-way mirroring on a matrix from top-left corner."""
    width = len(matrix[0])
    height = len(matrix)
    for i in range(height):
        for j in range(width):
            x = min(i, height - 1 - i)
            y = min(j, width - 1 - j)
            matrix[i][j] = matrix[x][y]
    return matrix


def gen_gaussian(width, height, factors):
    """Build a Gaussian blur kernel."""

    if width != height:
        raise CGBadArg("Cannot build an uneven Gaussian blur kernel.")

    size = width
    sigma = float(factors.get('sigma', 0.84089642))

    result = mbuild(size, size)
    for i in range(int(size / 2) + 1):
        for j in range(int(size / 2) + 1):
            diffx = i - int(size / 2)
            diffy = j - int(size / 2)
            result[i][j] = 1.0 / (2 * math.pi * sigma) \
                * pow(math.e, - (diffx * diffx + diffy * diffy) \
                / (2 * sigma * sigma))
    mnormalize(result)
    mmirror4(result)

    return result


def gen_box(width, height, factors):
    """Build a box blur kernel."""
    result = mbuild(width, height)
    for i in range(height):
        for j in range(width):
            result[i][j] = 1.0
    return result


def _gen_invalid(width, height, factors):
    '''Handle a convolution kernel generation request of an unrecognized type.'''
    raise CGBadArg("Unknown kernel type.")


def _args_readfactors(lst):
    """Parse the factor arguments."""
    factors = dict()
    if lst:
        for s in lst:
            res = s.partition('=')
            if not res[0]:
                raise CGBadArg("Factor has no key.")
            if not res[2]:
                raise CGBadArg("Factor has no value.")
            factors[res[0]] = float(res[2])
    return factors


def _parse_args():
    '''Parse the command-line arguments.'''

    parser = argparse.ArgumentParser(description='Build a convolution kernel.')
    parser.add_argument('type', help='Type of convolution kernel. May be "gaussian" (factor sigma = 0.84089642) or "box".')
    parser.add_argument('width', type=int, help='Width of convolution kernel. Must be an odd number.')
    parser.add_argument('height', nargs='?', type=int, help='Height of convolution kernel. Must be an odd number. Equals to width if omitted.')
    parser.add_argument('-f', '--factor', nargs='+', help='Factors of the convolution kernel, in name=value format.')
    parser.add_argument('--dump-compton', action='store_true', help='Dump in compton format.')
    return parser.parse_args()


def _main():
    args = _parse_args()

    width = args.width
    height = args.height
    if not height:
        height = width
    if not (width > 0 and height > 0):
        raise CGBadArg("Invalid width/height.")
    factors = _args_readfactors(args.factor)

    funcs = dict(gaussian=gen_gaussian, box=gen_box)
    matrix = (funcs.get(args.type, _gen_invalid))(width, height, factors)
    if args.dump_compton:
        mdumpcompton(matrix)
    else:
        mdump(matrix)


if __name__ == '__main__':
    _main()
