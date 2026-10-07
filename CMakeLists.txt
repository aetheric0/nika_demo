cmake_minimum_required(VERSION 3.22)

# MUST be included BEFORE project() to register the cross-compiler
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

project(nika)