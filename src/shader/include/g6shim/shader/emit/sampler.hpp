// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace g6shim::shader::emit {

struct StaticSampler {
    std::uint32_t filter = 0;
    std::array<std::uint32_t, 3> address{};
    float mip_lod_bias = 0.0f;
    std::uint32_t max_anisotropy = 0;
    std::uint32_t comparison = 0;
    std::uint32_t border = 0;
    float min_lod = 0.0f;
    float max_lod = 0.0f;

    static StaticSampler from_words(const std::array<std::uint32_t, 4>& words);

    friend bool operator==(const StaticSampler&, const StaticSampler&) = default;
};

}  // namespace g6shim::shader::emit
