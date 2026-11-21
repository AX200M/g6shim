# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_d3d11_x
    DLL "d3d11_x.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/d3d11_x.def"
    SOURCES ${sources})
target_link_libraries(hle_d3d11_x PRIVATE d3d11)
