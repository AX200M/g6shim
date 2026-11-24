# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_d3d12_xs
    DLL "d3d12_xs.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs.def"
    SOURCES ${sources} "${PROJECT_SOURCE_DIR}/hle/xg_xs/gfx10_layout.cpp")
target_include_directories(hle_d3d12_xs PRIVATE "${PROJECT_SOURCE_DIR}/hle/xg_xs")
target_link_libraries(hle_d3d12_xs PRIVATE d3d12 dxgi onecore)
