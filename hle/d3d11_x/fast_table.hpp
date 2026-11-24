// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "proxy.hpp"
#include "slots.hpp"

#include <cstddef>
#include <vector>

namespace g6shim::d3d {

inline constexpr std::size_t kFastTableOffset = 0x3E0;
inline constexpr std::size_t kCallTableFlagField = 0x390;
inline constexpr std::uintptr_t kRoutePacketsBit = 0x1;
inline constexpr std::uintptr_t kRouteSettersBit = 0x2;
inline constexpr std::uintptr_t kCallTableFlags = kRoutePacketsBit | kRouteSettersBit;

constexpr std::size_t fast_offset(std::size_t slot) {
    const std::size_t index = slot < context_slot::kIASetPrimitiveTopology ? slot : slot - 1;
    return kFastTableOffset + sizeof(void*) * index;
}

std::vector<Override> context_fields();

}  // namespace g6shim::d3d
