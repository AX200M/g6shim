# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(bvh_sources "${CMAKE_CURRENT_SOURCE_DIR}/bvh/format.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/bvh/builder.cpp")
g6shim_add_compute_dxil(g_bvh_build_dxil "${CMAKE_CURRENT_SOURCE_DIR}/bvh/build.hlsl" main)
g6shim_add_compute_dxil(g_present_plane_vs "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl" present_vs vs_6_0)
g6shim_add_compute_dxil(g_present_plane_ps "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl" present_ps ps_6_0)
g6shim_add_compute_dxil(g_depth_copy_vs "${CMAKE_CURRENT_SOURCE_DIR}/depth_copy.hlsl" copy_vs vs_6_0)
g6shim_add_compute_dxil(g_depth_copy_ps "${CMAKE_CURRENT_SOURCE_DIR}/depth_copy.hlsl" copy_depth_ps ps_6_0)
g6shim_add_compute_dxil(g_depth_copy_stencil_ps "${CMAKE_CURRENT_SOURCE_DIR}/depth_copy.hlsl" copy_stencil_ps ps_6_0)
g6shim_add_compute_dxil(g_depth_copy_stencil_bit_ps "${CMAKE_CURRENT_SOURCE_DIR}/depth_copy.hlsl" copy_stencil_bit_ps ps_6_0)
g6shim_add_hle_module(hle_d3d12_xs
    DLL "d3d12_xs.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs.def"
    SOURCES ${sources} ${bvh_sources} "${PROJECT_SOURCE_DIR}/hle/xg_xs/gfx10_layout.cpp")
target_include_directories(hle_d3d12_xs PRIVATE "${PROJECT_SOURCE_DIR}/hle/xg_xs")
target_link_libraries(hle_d3d12_xs PRIVATE d3d12 dxgi d3dcompiler onecore g6shim_shader)
g6shim_target_compute_dxil(hle_d3d12_xs g_bvh_build_dxil)
g6shim_target_compute_dxil(hle_d3d12_xs g_present_plane_vs)
g6shim_target_compute_dxil(hle_d3d12_xs g_present_plane_ps)
g6shim_target_compute_dxil(hle_d3d12_xs g_depth_copy_vs)
g6shim_target_compute_dxil(hle_d3d12_xs g_depth_copy_ps)
g6shim_target_compute_dxil(hle_d3d12_xs g_depth_copy_stencil_ps)
g6shim_target_compute_dxil(hle_d3d12_xs g_depth_copy_stencil_bit_ps)
