# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_g6shim_vccorlib140
    DLL "g6shim_vccorlib140.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/g6shim_vccorlib140.def"
    SOURCES ${sources})
target_link_libraries(hle_g6shim_vccorlib140 PRIVATE g6shim::winrt)
