# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_xcrdapi
    DLL "XCRDAPI.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/xcrdapi.def"
    SOURCES ${sources})
target_link_libraries(hle_xcrdapi PRIVATE hle_xmem g6shim_shader)
if(G6SHIM_FETCH_BCPACK_TABLES)
  g6shim_fetch_bcpack_tables(hle_xcrdapi)
endif()
