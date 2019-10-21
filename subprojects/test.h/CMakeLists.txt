cmake_minimum_required(VERSION 3.0)
project(test.h C)

add_library(test.h INTERFACE)
target_include_directories(test.h INTERFACE .)
