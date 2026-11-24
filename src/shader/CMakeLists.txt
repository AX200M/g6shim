# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_library(g6shim_shader STATIC
  dxbc.cpp
  dxil.cpp
  error.cpp
  gfx10.cpp
  gfx10_fields.cpp
  gfx10_sp3.cpp
  inflate.cpp
  native.cpp)
target_include_directories(g6shim_shader PUBLIC include)
