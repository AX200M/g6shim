// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace g6shim::d3d12_xs::descriptor_words {

using Words = std::array<std::uint32_t, 8>;

inline constexpr std::uint32_t kViewSlotBytes = 32;
inline constexpr std::uint32_t kSamplerSlotBytes = 16;

enum class ImageType : std::uint32_t {
    k1D = 8,
    k2D = 9,
    k3D = 10,
    kCube = 11,
    k1DArray = 12,
    k2DArray = 13,
};

struct ImageShape {
    ImageType type = ImageType::k2D;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::uint32_t levels = 1;
};

Words buffer(std::uint64_t address, std::uint32_t records, std::uint32_t stride);
Words image(const ImageShape& shape, std::uint64_t address);

}  // namespace g6shim::d3d12_xs::descriptor_words
