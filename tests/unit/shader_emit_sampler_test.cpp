// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <array>
#include <cstdint>

#include "g6shim/shader/emit/sampler.hpp"

namespace {

using namespace g6shim::shader::emit;

StaticSampler sampler(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2, std::uint32_t w3) {
    return StaticSampler::from_words({w0, w1, w2, w3});
}

TEST(sampler_console_compiled_samplers_decode) {
    const StaticSampler point = sampler(0x3088, 0x0054'0100, 0x1400'0080, 0x8000'0000);
    CHECK(point.filter == 0);
    CHECK((point.address == std::array<std::uint32_t, 3>{1, 2, 3}));
    CHECK(point.mip_lod_bias == 0.5f);
    CHECK(point.max_anisotropy == 1);
    CHECK(point.comparison == 4);
    CHECK(point.border == 2);
    CHECK(point.min_lod == 1.0f);
    CHECK(point.max_lod == 5.25f);

    const StaticSampler aniso = sampler(0x369E, 0x00FF'F000, 0x18F0'3E00, 0x8000'0000);
    CHECK(aniso.filter == 0x55);
    CHECK((aniso.address == std::array<std::uint32_t, 3>{4, 5, 3}));
    CHECK(aniso.max_anisotropy == 8);
    CHECK(aniso.mip_lod_bias == -2.0f);
    CHECK(aniso.max_lod == 4095.0f / 256.0f);

    const StaticSampler compare = sampler(0x3006, 0x00FF'F000, 0x1450'0000, 0x4000'0000);
    CHECK(compare.filter == 0x14);
    CHECK((compare.address == std::array<std::uint32_t, 3>{4, 1, 1}));
    CHECK(compare.border == 1);

    const StaticSampler minimum = sampler(0x2000'3003, 0x00FF'F000, 0x1850'0000, 0x8000'0000);
    CHECK(minimum.filter == 0x115);
    CHECK(minimum.address[0] == 5);

    const StaticSampler mag_mip = sampler(0x3002, 0, 0x1810'0000, 0x8000'0000);
    CHECK(mag_mip.filter == 0x05);
    CHECK(mag_mip.max_lod == 0.0f);

    const StaticSampler max_aniso = sampler(0x4000'3800, 0x00FF'F000, 0x18F0'0F00, 0x8000'0000);
    CHECK(max_aniso.filter == 0x1D5);
    CHECK(max_aniso.max_anisotropy == 16);
    CHECK(max_aniso.mip_lod_bias == 15.0f);
}

}  // namespace
