// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g6shim::xmem {

inline constexpr std::uint64_t kAllocationFieldShift = 24;
inline constexpr std::uint64_t kAllocationFieldMask = 0x1f;
inline constexpr std::uint64_t kMinimumAllocationField = 4;
inline constexpr std::uint64_t k32BitAllocation = std::uint64_t{1} << 33;

std::size_t allocation_alignment(std::uint64_t attributes);

}  // namespace g6shim::xmem
