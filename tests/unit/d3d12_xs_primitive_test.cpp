// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/primitive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace g6shim::d3d12_xs::primitive;

namespace {

std::vector<std::uint8_t> vertex(float x, float y, std::uint8_t red) {
    std::vector<std::uint8_t> value(12, 0);
    std::memcpy(value.data(), &x, sizeof x);
    std::memcpy(value.data() + 4, &y, sizeof y);
    value[8] = red;
    value[11] = 255;
    return value;
}

std::vector<std::uint8_t> vertex2(float x, float y) {
    std::vector<std::uint8_t> value(8, 0);
    std::memcpy(value.data(), &x, sizeof x);
    std::memcpy(value.data() + 4, &y, sizeof y);
    return value;
}

bool same(const std::vector<std::uint8_t>& whole, std::size_t offset, const std::vector<std::uint8_t>& part) {
    return std::equal(part.begin(), part.end(), whole.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<std::uint8_t> concat(const std::vector<std::vector<std::uint8_t>>& parts) {
    std::vector<std::uint8_t> all;
    for (const auto& part : parts) all.insert(all.end(), part.begin(), part.end());
    return all;
}

}  // namespace

TEST(d3d12_xs_rectangles_get_their_fourth_corner) {
    const std::vector<std::uint8_t> source =
        concat({vertex(-1.0f, 1.0f, 0), vertex(-1.0f, -1.0f, 100), vertex(1.0f, -1.0f, 150)});
    std::vector<std::uint8_t> destination(6 * 12, 0);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> elements{{0, 16}, {8, 28}};
    expand_rects(source, destination, 12, elements);
    CHECK(same(destination, 0, source));
    CHECK(same(destination, 36, std::vector<std::uint8_t>(source.begin(), source.begin() + 12)));
    CHECK(same(destination, 48, std::vector<std::uint8_t>(source.begin() + 24, source.begin() + 36)));
    CHECK(same(destination, 60, vertex(1.0f, 1.0f, 50)));
    CHECK(f32_to_half(1.5f) == 0x3e00);
    CHECK(half_to_f32(0x3e00) == 1.5f);
    CHECK(half_to_f32(0xc000) == -2.0f);
}

TEST(d3d12_xs_halves_round_up_into_the_next_power_of_two) {
    CHECK(f32_to_half(2.0f - std::ldexp(1.0f, -12)) == 0x4000);
    CHECK(f32_to_half(0.5f - std::ldexp(1.0f, -13)) == 0x3800);
    CHECK(f32_to_half(65520.0f) == 0x7c00);
    const std::vector<std::uint8_t> source{0x00, 0x3c, 0x00, 0x0c, 0x00, 0x3c};
    std::vector<std::uint8_t> destination(6 * 2, 0);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> elements{{0, 54}};
    expand_rects(source, destination, 2, elements);
    CHECK(destination[10] == 0x00 && destination[11] == 0x40);
}

TEST(d3d12_xs_elements_past_the_stride_are_left_alone) {
    const std::vector<std::uint8_t> source = concat({vertex2(0.0f, 2.0f), vertex2(1.0f, 3.0f), vertex2(5.0f, 7.0f)});
    std::vector<std::uint8_t> destination(6 * 8, 0);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> elements{{0, 16}, {4, 2}, {8, 28}};
    expand_rects(source, destination, 8, elements);
    const std::vector<std::uint8_t> expected =
        concat({source, std::vector<std::uint8_t>(source.begin(), source.begin() + 8),
                std::vector<std::uint8_t>(source.begin() + 16, source.begin() + 24)});
    CHECK(same(destination, 0, expected));
    CHECK(same(destination, 40, vertex2(4.0f, 6.0f)));
}

TEST(d3d12_xs_half_subnormals_convert_in_both_directions) {
    CHECK(f32_to_half(std::ldexp(1.0f, -24)) == 0x0001);
    CHECK(f32_to_half(-std::ldexp(1.0f, -24)) == 0x8001);
    CHECK(f32_to_half(std::ldexp(1.0f, -23)) == 0x0002);
    CHECK(f32_to_half(std::ldexp(1023.0f, -24)) == 0x03ff);
    CHECK(f32_to_half(std::ldexp(1.0f, -25)) == 0x0000);
    CHECK(f32_to_half(std::ldexp(3.0f, -25)) == 0x0002);
    CHECK(f32_to_half(std::ldexp(1.0f, -26)) == 0x0000);
    CHECK(f32_to_half(1.5f) == 0x3e00);
    CHECK(half_to_f32(0x0001) == std::ldexp(1.0f, -24));
}
