// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::d3d {

inline constexpr std::size_t kTopologyField = 0x170;
inline constexpr unsigned kPrimitiveTypeShift = 9;
inline constexpr unsigned kControlPointShift = 20;
inline constexpr std::uint32_t kSixBits = 0x3F;
inline constexpr unsigned kMaxControlPoints = 32;

enum PrimitiveType : std::uint32_t {
    kPoints = 1,
    kLines = 2,
    kLineStrip = 3,
    kTriangles = 4,
    kTriangleFan = 5,
    kTriangleStrip = 6,
    kPatches = 9,
    kLinesAdjacency = 10,
    kLineStripAdjacency = 11,
    kTrianglesAdjacency = 12,
    kTriangleStripAdjacency = 13,
};

inline D3D11_PRIMITIVE_TOPOLOGY decode_topology(std::uint64_t value) {
    const auto high = static_cast<std::uint32_t>(value >> 32);
    switch ((high >> kPrimitiveTypeShift) & kSixBits) {
        case kPoints: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case kLines: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case kLineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case kTriangles: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case kTriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case kLinesAdjacency: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        case kLineStripAdjacency: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
        case kTrianglesAdjacency: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        case kTriangleStripAdjacency: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        case kPatches: {
            const unsigned points = (high >> kControlPointShift) & kSixBits;
            if (points >= 1 && points <= kMaxControlPoints)
                return static_cast<D3D11_PRIMITIVE_TOPOLOGY>(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1);
            return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        }
        default: return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

}  // namespace g6shim::d3d
