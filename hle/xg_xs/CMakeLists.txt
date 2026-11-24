# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_xg_xs
    DLL "xg_xs.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/xg_xs.def"
    SOURCES ${sources})
