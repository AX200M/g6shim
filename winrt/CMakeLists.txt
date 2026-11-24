# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(g6shim_winrt STATIC ${sources})
add_library(g6shim::winrt ALIAS g6shim_winrt)
target_include_directories(g6shim_winrt PUBLIC include)
target_link_libraries(g6shim_winrt PUBLIC g6shim::runtime runtimeobject dinput8 dxguid)
