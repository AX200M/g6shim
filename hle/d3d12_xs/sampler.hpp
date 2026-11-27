// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace g6shim::d3d12_xs::sampler {

inline constexpr std::size_t kConsoleWords = 27;

D3D12_SAMPLER_DESC console(const std::array<std::uint32_t, kConsoleWords>& words);

}  // namespace g6shim::d3d12_xs::sampler
