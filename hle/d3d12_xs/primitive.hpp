// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace g6shim::d3d12_xs::primitive {

void expand_rects(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination, std::size_t stride,
                  std::span<const std::pair<std::uint32_t, std::uint32_t>> elements);
std::uint16_t f32_to_half(float value);
float half_to_f32(std::uint16_t value);

}  // namespace g6shim::d3d12_xs::primitive
