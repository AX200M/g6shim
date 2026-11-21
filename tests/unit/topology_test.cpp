// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "topology.hpp"

namespace {

using namespace g6shim::d3d;

std::uint64_t field(std::uint32_t primitive, std::uint32_t control_points = 0, std::uint32_t low = 0) {
    const std::uint32_t high = (primitive << kPrimitiveTypeShift) | (control_points << kControlPointShift);
    return (std::uint64_t{high} << 32) | low;
}

}  // namespace

TEST(topology_decodes_primitive_types) {
    CHECK(decode_topology(field(kPoints)) == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    CHECK(decode_topology(field(kLines)) == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    CHECK(decode_topology(field(kLineStrip)) == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    CHECK(decode_topology(field(kTriangles)) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CHECK(decode_topology(field(kTriangleStrip)) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    CHECK(decode_topology(field(kLinesAdjacency)) == D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ);
    CHECK(decode_topology(field(kLineStripAdjacency)) == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ);
    CHECK(decode_topology(field(kTrianglesAdjacency)) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
    CHECK(decode_topology(field(kTriangleStripAdjacency)) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ);
}

TEST(topology_observed_triangle_list_value) {
    CHECK(decode_topology(0x9FFC0009E00ull) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

TEST(topology_ignores_the_low_dword) {
    CHECK(decode_topology(field(kTriangles, 0, 0xFFFFFFFF)) == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

TEST(topology_patch_lists) {
    CHECK(decode_topology(field(kPatches, 1)) == D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
    CHECK(decode_topology(field(kPatches, 16)) == D3D11_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST);
    CHECK(decode_topology(field(kPatches, 32)) == D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
    CHECK(decode_topology(field(kPatches, 0)) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
    CHECK(decode_topology(field(kPatches, 33)) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
}

TEST(topology_unknown_and_console_only_types) {
    CHECK(decode_topology(0) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
    CHECK(decode_topology(field(kTriangleFan)) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
    CHECK(decode_topology(field(7)) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
    CHECK(decode_topology(field(0x3F)) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
}
