# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)

function(g6shim_require_opus)
  if(TARGET Opus::opus)
    return()
  endif()
  FetchContent_Declare(opus
    URL "https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz"
    URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(opus)
  get_target_property(options opus COMPILE_OPTIONS)
  if(options)
    list(REMOVE_ITEM options /W4 /permissive-)
    set_target_properties(opus PROPERTIES COMPILE_OPTIONS "${options}")
  endif()
endfunction()
