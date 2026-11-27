// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <span>

namespace g6shim::xgameruntime::xuser {

inline constexpr Guid kProvider = guid(0x01acd177, 0x91f9, 0x4763, 0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32,
                                       0xe0);
inline constexpr Guid kIid = guid(0xeb9bf948, 0x18dc, 0x4d82, 0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0);
inline constexpr Guid kGamertagIid = guid(0xcef4fac0, 0x7676, 0x4a94, 0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b,
                                          0x74);
inline constexpr Guid kDeviceProvider = guid(0x7d824997, 0x10dc, 0x45ab, 0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c,
                                             0x0b, 0xf1);
inline constexpr Guid kDeviceIid = guid(0x0cc6a956, 0xe7e1, 0x4fdf, 0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb,
                                        0xc8);

inline constexpr HResult kUserNotFound = static_cast<HResult>(0x89245104u);
inline constexpr HResult kFailedToGetToken = static_cast<HResult>(0x89245111u);

inline constexpr std::uint64_t kXuid = 0x0009000000000001ull;
inline constexpr std::uint64_t kLocalId = 1;
inline constexpr std::uint32_t kModernSuffixComponent = 2;
inline constexpr std::uint32_t kAdultAgeGroup = 3;

extern const Object kObject;
extern const Object kGamertagObject;
extern const Object kDeviceObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xuser
