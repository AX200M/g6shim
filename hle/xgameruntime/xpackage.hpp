// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace g6shim::xgameruntime::xpackage {

inline constexpr Guid kProvider = guid(0xaf406016, 0xe850, 0x4aa8, 0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac,
                                       0x7e);
inline constexpr Guid kIid2026 = guid(0x0b9d556e, 0xab70, 0x43f4, 0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0,
                                      0x40);

inline constexpr HResult kNoPackage = static_cast<HResult>(0x89245208u);

struct Progress {
    std::uint64_t total_bytes;
    std::uint64_t installed_bytes;
    std::uint64_t launch_bytes;
    bool launchable;
    bool completed;
};

static_assert(sizeof(Progress) == 32);

extern const Object kObject;

std::string identifier();
const void* monitor();
std::uint64_t package_bytes();

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xpackage
