// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/sampler.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace g6shim::shader::emit {

namespace {

constexpr std::uint32_t bits(std::uint32_t word, std::uint32_t at, std::uint32_t count) {
    return word >> at & ((1u << count) - 1u);
}

constexpr std::array<std::uint32_t, 8> kAddressModes = {1, 2, 3, 5, 3, 5, 4, 5};

constexpr std::uint32_t kAnisotropicFilter = 0x55;

constexpr float kLodScale = 256.0f;

}  // namespace

StaticSampler StaticSampler::from_words(const std::array<std::uint32_t, 4>& w) {
    const std::uint32_t mag = bits(w[2], 20, 2);
    const std::uint32_t min = bits(w[2], 22, 2);
    const std::uint32_t mip = bits(w[2], 26, 2);
    std::uint32_t reduction = 0;
    switch (bits(w[0], 29, 2)) {
        case 1: reduction = 2; break;
        case 2: reduction = 3; break;
        default: break;
    }
    StaticSampler sampler;
    if (mag >= 2 || min >= 2) {
        sampler.filter = kAnisotropicFilter | reduction << 7;
    } else {
        sampler.filter = (min & 1u) << 4 | (mag & 1u) << 2 | static_cast<std::uint32_t>(mip == 2) |
                         reduction << 7;
    }
    const auto lod = [](std::uint32_t value) { return static_cast<float>(value) / kLodScale; };
    if (mip == 0) {
        sampler.min_lod = 0.0f;
        sampler.max_lod = 0.0f;
    } else {
        sampler.min_lod = lod(bits(w[1], 0, 12));
        sampler.max_lod = lod(bits(w[1], 12, 12));
    }
    for (std::size_t i = 0; i < sampler.address.size(); ++i)
        sampler.address[i] = kAddressModes[bits(w[0], static_cast<std::uint32_t>(i) * 3, 3)];
    const std::int32_t bias = std::bit_cast<std::int32_t>(bits(w[2], 0, 14) << 18) >> 18;
    sampler.mip_lod_bias = static_cast<float>(bias) / kLodScale;
    sampler.max_anisotropy = 1u << std::min(bits(w[0], 9, 3), 4u);
    sampler.comparison = bits(w[0], 12, 3) + 1;
    sampler.border = bits(w[3], 30, 2) % 3;
    return sampler;
}

}  // namespace g6shim::shader::emit
