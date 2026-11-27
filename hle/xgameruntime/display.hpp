// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <span>

namespace g6shim::xgameruntime::display {

inline constexpr Guid kHdrProvider = guid(0x03f0fe74, 0xfdd9, 0x4e5c, 0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac,
                                          0xc5);
inline constexpr Guid kHdrIid = guid(0x35f07670, 0x706e, 0x4bfb, 0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3);
inline constexpr Guid kShell = guid(0x1b339674, 0x328d, 0x4283, 0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36,
                                    0x39);
inline constexpr Guid kCaptureProvider = guid(0xa4f1aee2, 0x4bf1, 0x4485, 0xb0, 0x08, 0xa7, 0xc2, 0x6d, 0x52,
                                              0xac, 0x27);
inline constexpr Guid kCaptureIid2026 = guid(0x22e672d7, 0xb4e3, 0x406c, 0xbd, 0x50, 0x8f, 0x0d, 0x25, 0x23,
                                             0x6f, 0x9e);

inline constexpr std::uint32_t kHdrDisabled = 2;

extern const Object kHdrObject;
extern const Object kShellObject;
extern const Object kCaptureObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::display
