# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
add_library(g6shim_runtime SHARED src/log.cpp src/window.cpp)
add_library(g6shim::runtime ALIAS g6shim_runtime)

target_include_directories(g6shim_runtime PUBLIC include)
target_compile_definitions(g6shim_runtime PRIVATE G6SHIM_RUNTIME_BUILD)
set_target_properties(g6shim_runtime PROPERTIES PREFIX "")
