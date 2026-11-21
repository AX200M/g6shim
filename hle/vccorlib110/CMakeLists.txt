# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_vccorlib110
    DLL "vccorlib110.DLL"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/vccorlib110.def"
    SOURCES ${sources})
