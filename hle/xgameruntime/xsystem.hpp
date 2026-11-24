// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace g6shim::xgameruntime::xsystem {

inline constexpr Guid kProvider = guid(0xe349bd1a, 0xfc20, 0x4e40, 0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40,
                                       0x9f);
inline constexpr Guid kIid = guid(0x6fd71f09, 0x7513, 0x49f0, 0x89, 0xbc, 0xbf, 0xaf, 0x5d, 0xf6, 0xf8, 0x52);
inline constexpr Guid kIid2026 = guid(0x1861cf2e, 0xe18b, 0x4834, 0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4,
                                      0xcf);
inline constexpr Guid kAnalytics = guid(0xb884675d, 0xb738, 0x4a9c, 0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c,
                                        0x9b);

inline constexpr char kConsoleId[] = "00000000000000000000000000000000000001";
inline constexpr char kSandbox[] = "RETAIL";
inline constexpr char kDeviceId[] = "VkljdG9yeSBvZmZsaW5lIGRldmljZSBpZGVudGl0eSE=";
inline constexpr char kFamily[] = "Xbox";
inline constexpr char kForm[] = "Xbox Series X";

struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t build;
    std::uint16_t revision;
};

struct AnalyticsInfo {
    Version os;
    Version hosting_os;
    std::array<char, 64> family;
    std::array<char, 64> form;
};

inline constexpr Version kOsVersion = {10, 0, 22000, 0};

static_assert(sizeof(Version) == 8);
static_assert(sizeof(AnalyticsInfo) == 0x90);

extern const Object kObject;
extern const Object kAnalyticsObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xsystem
