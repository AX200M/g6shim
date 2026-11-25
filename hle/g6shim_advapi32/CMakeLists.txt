# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_g6shim_advapi32
    DLL "g6shim_advapi32.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/g6shim_advapi32.def"
    SOURCES ${sources})
