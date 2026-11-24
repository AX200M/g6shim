// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <string_view>

namespace g6shim::winrt {

inline constexpr std::size_t kMaxManifestBytes = 1 << 20;
inline constexpr std::size_t kMaxAttributeValue = 256;
inline constexpr std::size_t kMaxInProcessServers = 64;
inline constexpr std::size_t kMaxClassName = 256;

std::wstring game_folder();

const std::string& manifest_text();

std::string manifest_attribute(std::string_view text, std::string_view element, std::string_view attribute);

std::map<std::string, std::string> parse_in_process_servers(std::string_view text);

}  // namespace g6shim::winrt
