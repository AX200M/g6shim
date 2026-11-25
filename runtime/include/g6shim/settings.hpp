// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace g6shim::settings {

inline constexpr std::size_t kMaxVariableChars = 64;
inline constexpr std::size_t kMaxValueBytes = 32768;
inline constexpr std::size_t kMaxWideChars = 32767;

G6SHIM_RUNTIME_API std::size_t copy(const char* variable, std::size_t variable_length, char* buffer,
                                    std::size_t buffer_size);
G6SHIM_RUNTIME_API std::size_t copy_wide(const char* variable, std::size_t variable_length, wchar_t* buffer,
                                         std::size_t buffer_size);

inline std::optional<std::string> get(std::string_view variable) {
    if (variable.empty() || variable.size() > kMaxVariableChars) return std::nullopt;
    const std::size_t needed = copy(variable.data(), variable.size(), nullptr, 0);
    if (needed == 0 || needed > kMaxValueBytes) return std::nullopt;
    std::string out(needed, '\0');
    if (copy(variable.data(), variable.size(), out.data(), out.size()) != needed) return std::nullopt;
    return out;
}

inline std::optional<std::wstring> get_wide(std::string_view variable) {
    if (variable.empty() || variable.size() > kMaxVariableChars) return std::nullopt;
    const std::size_t needed = copy_wide(variable.data(), variable.size(), nullptr, 0);
    if (needed == 0 || needed > kMaxWideChars) return std::nullopt;
    std::wstring out(needed, L'\0');
    if (copy_wide(variable.data(), variable.size(), out.data(), out.size()) != needed) return std::nullopt;
    return out;
}

inline bool enabled(std::string_view variable) {
    const std::optional<std::string> value = get(variable);
    return value && !value->empty() && value->front() != '0';
}

}  // namespace g6shim::settings
