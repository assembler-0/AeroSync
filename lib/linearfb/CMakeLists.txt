cmake_minimum_required(VERSION 3.10)
project(linearfb C)
add_library(linearfb STATIC linearfb.c font.c)
target_include_directories(linearfb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
