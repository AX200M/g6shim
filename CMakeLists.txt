# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(g6shim VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "g6shim builds with MSVC on Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(G6SHIM_BUILD_TOOLS "Build the command-line tools" ON)
option(G6SHIM_BUILD_HLE "Build the replacement DLLs" ON)
option(G6SHIM_BUILD_TESTS "Build the tests" ON)
set(G6SHIM_HLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hle" CACHE PATH "Folder holding the replacement DLL projects")
set(G6SHIM_GAME_DIR "" CACHE PATH "Game folder the g6shim_deploy target copies the replacement DLLs into")

add_compile_options(/W4 /permissive- /utf-8)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/g6shim.cmake)

add_subdirectory(runtime)
add_subdirectory(winrt EXCLUDE_FROM_ALL)

if(G6SHIM_BUILD_TOOLS)
  add_subdirectory(src)
endif()

if(G6SHIM_BUILD_HLE)
  g6shim_add_hle_dir("${G6SHIM_HLE_DIR}")
endif()

if(G6SHIM_BUILD_TOOLS AND G6SHIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
