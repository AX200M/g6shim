// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <span>

namespace g6shim::xgameruntime::xnetworking {

inline constexpr Guid kProvider = guid(0x37e56907, 0x2f10, 0x41e8, 0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33,
                                       0x1a);
inline constexpr Guid kIid2026 = guid(0xbf2346b2, 0x39af, 0x4658, 0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83,
                                      0xb3);

inline constexpr HResult kUnreachable = static_cast<HResult>(0x800704CFu);
inline constexpr std::uint16_t kUdpPort = 3074;
inline constexpr std::uint32_t kOfflineLevel = 1;

struct ConnectivityHint {
    std::uint32_t level;
    std::uint32_t cost;
    std::uint32_t interface_type;
    bool initialized;
    bool approaching_data_limit;
    bool over_data_limit;
    bool roaming;
};

static_assert(sizeof(ConnectivityHint) == 16);

extern const Object kObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xnetworking
