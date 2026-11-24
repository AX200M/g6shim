// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace g6shim::shader {

std::vector<std::uint8_t> inflate_zlib(std::span<const std::uint8_t> stream, std::size_t limit);
std::vector<std::uint8_t> inflate_raw(std::span<const std::uint8_t> stream, std::size_t limit);

}  // namespace g6shim::shader
