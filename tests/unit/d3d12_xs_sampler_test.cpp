// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/sampler.hpp"

#include <array>
#include <cstdint>
#include <cstring>

TEST(d3d12_xs_console_sampler_descriptions_decode) {
    std::array<std::uint32_t, g6shim::d3d12_xs::sampler::kConsoleWords> words{};
    words[1] = 1;
    words[2] = 0;
    words[3] = 2;
    words[6] = 3;
    words[7] = 3;
    words[8] = 3;
    const float max_lod = 1000.0f;
    std::memcpy(&words[10], &max_lod, sizeof max_lod);
    const D3D12_SAMPLER_DESC desc = g6shim::d3d12_xs::sampler::console(words);
    CHECK(static_cast<int>(desc.Filter) == 0x5);
    CHECK(desc.AddressW == D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CHECK(desc.MaxLOD == 1000.0f);
    words[16] = 4;
    CHECK(static_cast<int>(g6shim::d3d12_xs::sampler::console(words).Filter) == 0x85);
    CHECK(g6shim::d3d12_xs::sampler::console(words).ComparisonFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL);
    words[16] = 1;
    CHECK(static_cast<int>(g6shim::d3d12_xs::sampler::console(words).Filter) == 0x5);
}
