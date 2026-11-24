# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(g6shim-scan main.cpp)
target_link_libraries(g6shim-scan PRIVATE g6shim_core)
target_compile_definitions(g6shim-scan PRIVATE G6SHIM_VERSION="${PROJECT_VERSION}")
