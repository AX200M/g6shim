// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "format.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace g6shim::d3d12_xs::bvh {

namespace {

constexpr std::uint64_t kBox16SpareUnit = 64;
constexpr std::uint64_t kLargestEnd = (std::uint64_t{1} << 32) - kUnit;
constexpr std::uint64_t kPointerUnits = std::uint64_t{1} << 29;
constexpr std::uint64_t kScratchHeader = 64;
constexpr std::uint64_t kScratchPerKey = 16;
constexpr std::uint64_t kScratchPerElementBox = 24;
constexpr std::uint64_t kScratchPerAccumulator = 32;
constexpr std::uint64_t kScratchPerInnerNode = 20;
constexpr std::uint64_t kScratchPerTileCount = 8;
constexpr std::uint64_t kScratchPerElementPointer = 4;

constexpr std::uint32_t kFloatSign = 0x80000000u;
constexpr std::uint32_t kFloatMagnitude = 0x7FFFFFFFu;
constexpr std::uint32_t kFloatInfinity = 0x7F800000u;
constexpr std::uint32_t kFloatMantissa = 0x7FFFFFu;
constexpr std::uint32_t kFloatMantissaBits = 23;
constexpr int kFloatBias = 127;
constexpr std::uint32_t kHalfSign = 0x8000u;
constexpr std::uint32_t kHalfNan = 0x7E00u;
constexpr std::uint32_t kHalfInfinity = 0x7C00u;
constexpr std::uint32_t kHalfMax = 0x7BFFu;
constexpr std::uint32_t kHalfSmallestNormal = 0x0400u;
constexpr std::uint32_t kHalfMantissaBits = 10;
constexpr std::uint32_t kHalfExponentMask = 0x1Fu;
constexpr std::uint32_t kHalfMantissaMask = 0x3FFu;
constexpr int kHalfBias = 15;
constexpr int kHalfLargestExponent = 16;
constexpr int kHalfSmallestExponent = -14;
constexpr std::uint32_t kDroppedMantissa = 0x1FFFu;

}

bool compute_layout(std::uint32_t count, bool top_level, bool box16, Layout* out) {
    Layout layout{};
    layout.top_level = top_level;
    layout.box16 = box16;
    layout.node_size = box16 ? kBox16Size : kBox32Size;
    layout.elements = count;
    layout.element_size = top_level ? kInstanceSize : kLeafSize;
    layout.nodes = count <= 1 ? 1u : static_cast<std::uint32_t>((3ull * count - 2) / 4);
    const std::uint64_t pad = box16 ? kBox16SpareUnit : 0;
    const std::uint64_t body_units =
        (std::uint64_t{count} * layout.element_size + std::uint64_t{layout.nodes} * layout.node_size + pad) / kUnit;
    const std::uint64_t parent_bytes = (4 * body_units + kUnit - 1) / kUnit * kUnit;
    const std::uint64_t elements_offset = kHeaderSize + parent_bytes;
    const std::uint64_t nodes_offset = elements_offset + std::uint64_t{count} * layout.element_size;
    const std::uint64_t end = nodes_offset + std::uint64_t{layout.nodes} * layout.node_size + pad;
    if (end > kLargestEnd || (end / kUnit) >= kPointerUnits) return false;
    layout.parents_offset = kHeaderSize;
    layout.elements_offset = static_cast<std::uint32_t>(elements_offset);
    layout.nodes_offset = static_cast<std::uint32_t>(nodes_offset);
    layout.size = static_cast<std::uint32_t>(end);
    layout.parent_entries = static_cast<std::uint32_t>(body_units);
    layout.sort_size = count;
    layout.scratch = kScratchHeader + kScratchPerKey * layout.sort_size +
                     kScratchPerTileCount * kRadixBins * radix_tiles(layout) + kScratchPerElementBox * count +
                     kScratchPerAccumulator * layout.nodes + kScratchPerInnerNode * inner_nodes(layout) +
                     kScratchPerTileCount * kept_tiles(layout) + kScratchPerElementPointer * count;
    *out = layout;
    return true;
}

std::uint32_t radix_tiles(const Layout& layout) { return (layout.sort_size + kRadixTile - 1) / kRadixTile; }

std::uint32_t inner_nodes(const Layout& layout) { return layout.elements > 1 ? layout.elements - 1 : 0; }

std::uint32_t kept_tiles(const Layout& layout) { return (inner_nodes(layout) + kRadixTile - 1) / kRadixTile; }

std::uint16_t half_directed(float value, bool up) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & kHalfSign;
    const std::uint32_t magnitude = bits & kFloatMagnitude;
    const bool away = up != (sign != 0);
    if (magnitude > kFloatInfinity) return static_cast<std::uint16_t>(sign | kHalfNan);
    if (magnitude == 0) return static_cast<std::uint16_t>(sign);
    const int exponent = static_cast<int>(magnitude >> kFloatMantissaBits) - kFloatBias;
    if (exponent >= kHalfLargestExponent)
        return static_cast<std::uint16_t>(sign | (away ? kHalfInfinity : kHalfMax));
    if (exponent < kHalfSmallestExponent) return static_cast<std::uint16_t>(away ? (sign | kHalfSmallestNormal) : sign);
    const std::uint32_t mantissa = magnitude & kFloatMantissa;
    std::uint32_t half = static_cast<std::uint32_t>(exponent + kHalfBias) << kHalfMantissaBits |
                         (mantissa >> (kFloatMantissaBits - kHalfMantissaBits));
    if (away && (mantissa & kDroppedMantissa) != 0) half += 1;
    return static_cast<std::uint16_t>(sign | half);
}

float half_to_float(std::uint16_t bits) {
    const std::uint32_t sign = (static_cast<std::uint32_t>(bits) & kHalfSign) << 16;
    const std::uint32_t exponent = (bits >> kHalfMantissaBits) & kHalfExponentMask;
    const std::uint32_t mantissa = bits & kHalfMantissaMask;
    if (exponent == kHalfExponentMask)
        return std::bit_cast<float>(sign | kFloatInfinity | (mantissa << (kFloatMantissaBits - kHalfMantissaBits)));
    if (exponent == 0) {
        const float magnitude = std::ldexp(static_cast<float>(mantissa), kHalfSmallestExponent - 10);
        return sign != 0 ? -magnitude : magnitude;
    }
    return std::bit_cast<float>(sign | (exponent - kHalfBias + kFloatBias) << kFloatMantissaBits |
                                mantissa << (kFloatMantissaBits - kHalfMantissaBits));
}

void encode_box16(const float lo[3], const float hi[3], std::uint32_t out[3]) {
    out[0] = half_directed(lo[0], false) | static_cast<std::uint32_t>(half_directed(lo[1], false)) << 16;
    out[1] = half_directed(lo[2], false) | static_cast<std::uint32_t>(half_directed(hi[0], true)) << 16;
    out[2] = half_directed(hi[1], true) | static_cast<std::uint32_t>(half_directed(hi[2], true)) << 16;
}

void decode_box16(const std::uint32_t words[3], float lo[3], float hi[3]) {
    const std::uint16_t halves[6] = {
        static_cast<std::uint16_t>(words[0]), static_cast<std::uint16_t>(words[0] >> 16),
        static_cast<std::uint16_t>(words[1]), static_cast<std::uint16_t>(words[1] >> 16),
        static_cast<std::uint16_t>(words[2]), static_cast<std::uint16_t>(words[2] >> 16)};
    constexpr float kLargest = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = std::clamp(half_to_float(halves[axis]), -kLargest, kLargest);
        hi[axis] = std::clamp(half_to_float(halves[3 + axis]), -kLargest, kLargest);
    }
}

}  // namespace g6shim::d3d12_xs::bvh
