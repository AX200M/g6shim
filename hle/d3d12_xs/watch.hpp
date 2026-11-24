// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::watch {

inline constexpr std::size_t kPageBytes = 64 << 10;
inline constexpr std::size_t kKernelPageBytes = 4096;

struct Pages {
    std::uint64_t asked = 0;
    std::vector<std::pair<std::size_t, std::uint64_t>> written;
};

std::optional<Pages> writes(std::size_t start, std::size_t end, bool guarded);
void writing(std::size_t at, std::size_t bytes);
void title_memory_remapped(std::size_t at, std::size_t bytes);

}  // namespace g6shim::d3d12_xs::watch
