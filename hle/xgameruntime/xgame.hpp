// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::xgameruntime::xgame {

inline constexpr Guid kTitleProvider = guid(0x973a344e, 0x24bf, 0x4d0f, 0x84, 0x57, 0x56, 0xc5, 0x34, 0x89,
                                            0x2b, 0x29);
inline constexpr Guid kTitleIid = guid(0x2549f142, 0x6419, 0x4a06, 0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34);
inline constexpr Guid kFeatures = guid(0x8836fe87, 0xedb9, 0x4fe3, 0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b,
                                       0x40);
inline constexpr Guid kInvites = guid(0x0651aae2, 0x4012, 0x4077, 0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e,
                                      0x2c);
inline constexpr Guid kEvents = guid(0xbbfbdcc7, 0xbfe7, 0x409b, 0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b,
                                     0x4d);

inline constexpr std::size_t kMaxManifestBytes = std::size_t{1} << 20;

inline constexpr std::array<std::uint32_t, 20> kAvailableFeatures = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};

extern const Object kTitleObject;
extern const Object kFeaturesObject;
extern const Object kInvitesObject;
extern const Object kEventsObject;

std::uint32_t title_id();
std::optional<std::string> title_manifest();
std::optional<std::string> manifest_element(std::string_view text, std::string_view path);
std::optional<std::uint32_t> manifest_title_id(std::string_view text);
std::vector<std::string> relaunch_args(const std::vector<std::string>& ours, std::string_view current,
                                       std::string_view exe, std::string_view args);

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xgame
