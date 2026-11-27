// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "descriptor_words.hpp"

#include <algorithm>

namespace g6shim::d3d12_xs::descriptor_words {

namespace {

constexpr std::uint32_t kIdentitySwizzle = 0xFAC;
constexpr std::uint32_t kAddressHighMask = 0xFFFF;
constexpr std::uint32_t kStrideMask = 0x3FFF;
constexpr std::uint32_t kStrideShift = 16;
constexpr std::uint32_t kImageAddressShift = 8;
constexpr std::uint32_t kImageAddressHighShift = 40;
constexpr std::uint32_t kImageAddressHighMask = 0xFF;
constexpr std::uint32_t kWidthLowShift = 30;
constexpr std::uint32_t kWidthLowBits = 2;
constexpr std::uint32_t kExtentMask = 0x3FFF;
constexpr std::uint32_t kHeightShift = 14;
constexpr std::uint32_t kLastLevelShift = 16;
constexpr std::uint32_t kLastLevelMask = 0xF;
constexpr std::uint32_t kTypeShift = 28;
constexpr std::uint32_t kDepthMask = 0x1FFF;

std::uint32_t less_one(std::uint32_t value) { return std::max<std::uint32_t>(value, 1) - 1; }

}  // namespace

Words buffer(std::uint64_t address, std::uint32_t records, std::uint32_t stride) {
    Words out{};
    out[0] = static_cast<std::uint32_t>(address);
    out[1] = (static_cast<std::uint32_t>(address >> 32) & kAddressHighMask) | (stride & kStrideMask) << kStrideShift;
    out[2] = records;
    out[3] = kIdentitySwizzle;
    return out;
}

Words image(const ImageShape& shape, std::uint64_t address) {
    const std::uint32_t width = less_one(shape.width);
    Words out{};
    out[0] = static_cast<std::uint32_t>(address >> kImageAddressShift);
    out[1] = (static_cast<std::uint32_t>(address >> kImageAddressHighShift) & kImageAddressHighMask) |
             (width & ((1u << kWidthLowBits) - 1)) << kWidthLowShift;
    out[2] = (width >> kWidthLowBits & kExtentMask) | (less_one(shape.height) & kExtentMask) << kHeightShift;
    out[3] = kIdentitySwizzle | (less_one(shape.levels) & kLastLevelMask) << kLastLevelShift |
             static_cast<std::uint32_t>(shape.type) << kTypeShift;
    out[4] = less_one(shape.depth) & kDepthMask;
    return out;
}

}  // namespace g6shim::d3d12_xs::descriptor_words
