# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(g6shim-shader main.cpp)
target_link_libraries(g6shim-shader PRIVATE g6shim_shader)
