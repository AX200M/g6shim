// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <utility>

namespace g6shim::d3d12_xs {

enum class Console { kSeriesX, kSeriesS };

inline constexpr std::size_t kGibibyte = std::size_t{1} << 30;

constexpr std::pair<std::size_t, std::size_t> title_memory(Console console) {
    if (console == Console::kSeriesS) return {0, std::size_t{7776} << 20};
    return {10 * kGibibyte, 7 * kGibibyte / 2};
}

constexpr const char* console_name(Console console) {
    return console == Console::kSeriesS ? "Xbox Series S" : "Xbox Series X";
}

void set_console(Console console);
Console current_console();

}  // namespace g6shim::d3d12_xs
