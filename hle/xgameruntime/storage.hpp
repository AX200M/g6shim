// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace g6shim::xgameruntime::storage {

inline constexpr Guid kProvider = guid(0xf4faf4d4, 0x2d04, 0x4fce, 0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e,
                                       0x84);
inline constexpr Guid kIid = guid(0xd29411df, 0x0794, 0x4553, 0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d);
inline constexpr Guid kIid2026 = guid(0x41a4e10c, 0x5a7e, 0x41d9, 0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07,
                                      0xd6);

inline constexpr HResult kFail = static_cast<HResult>(0x80004005u);

struct SpaceInfo {
    std::uint64_t available_free;
    std::uint64_t total_free;
    std::uint64_t used;
    std::uint64_t total;
};

static_assert(sizeof(SpaceInfo) == 32);

extern const Object kObject;

std::optional<std::string> drive_path();
std::uint64_t used_bytes(const std::filesystem::path& folder);
std::uint64_t disk_free(const std::filesystem::path& folder);
SpaceInfo space();

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::storage
