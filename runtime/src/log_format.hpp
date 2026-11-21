// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace g6shim::detail {

inline constexpr std::size_t kTimestampWidth = 9;
inline constexpr std::uint64_t kMicrosecondsPerMillisecond = 1000;
inline constexpr std::uint64_t kMillisecondsPerSecond = 1000;
inline constexpr std::uint64_t kRoundToMillisecond = kMicrosecondsPerMillisecond / 2;

inline std::string format_elapsed(std::chrono::microseconds elapsed) {
    const auto microseconds = static_cast<std::uint64_t>(elapsed.count() < 0 ? 0 : elapsed.count());
    const std::uint64_t milliseconds = (microseconds + kRoundToMillisecond) / kMicrosecondsPerMillisecond;
    const std::uint64_t seconds = milliseconds / kMillisecondsPerSecond;
    const std::uint64_t fraction = milliseconds % kMillisecondsPerSecond;

    std::string text = std::to_string(seconds);
    text.push_back('.');
    text.push_back(static_cast<char>('0' + fraction / 100));
    text.push_back(static_cast<char>('0' + (fraction / 10) % 10));
    text.push_back(static_cast<char>('0' + fraction % 10));
    if (text.size() < kTimestampWidth) text.insert(0, kTimestampWidth - text.size(), ' ');
    return text;
}

}  // namespace g6shim::detail
