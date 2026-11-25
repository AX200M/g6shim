// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace g6shim::xgameruntime::xgameui {

inline constexpr Guid kProvider = guid(0xdfcd4649, 0x4ff8, 0x4043, 0xba, 0x07, 0x35, 0xd6, 0x07, 0xdf, 0x98,
                                       0xb0);
inline constexpr Guid kIid2026 = guid(0xeaf669df, 0x5542, 0x4590, 0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37,
                                      0xcc);

using Ask = std::optional<std::string> (*)(const std::string& title, const std::string& description,
                                           const std::string& initial, std::uint32_t max_length);

Ask& ask();

std::string limit_text(std::string_view typed, std::uint32_t max_length);

extern const Object kObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xgameui
