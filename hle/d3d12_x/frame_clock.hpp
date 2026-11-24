// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace g6shim::d3d12 {

inline constexpr std::uint32_t kFrameEventTypes = 3;
inline constexpr std::uint32_t kFrameEventOrigin = 0;
inline constexpr std::uint32_t kFrameEventVblank = 1;
inline constexpr std::uint32_t kFrameEventFlip = 2;
inline constexpr std::uint32_t kScheduleFlagDisableSignal = 1;
inline constexpr std::uint32_t kDefaultFrameIntervalMicroseconds = 16667;
inline constexpr std::uint32_t kMaxScheduledObjects = 16;
inline constexpr std::uint32_t kFrameTokenTypeShift = 56;

struct FrameClock {
    std::uint32_t interval_us = kDefaultFrameIntervalMicroseconds;
    std::uint32_t origin_period = 1;
    std::array<std::uint32_t, kFrameEventTypes> offset_us{};

    std::uint64_t every_us(std::uint32_t type) const {
        return std::uint64_t(interval_us) * (type == kFrameEventOrigin ? origin_period : 1);
    }

    std::uint64_t event_time_us(std::uint32_t type, std::uint64_t index) const {
        return offset_us[type] + index * every_us(type);
    }

    std::uint64_t first_index_at_or_after(std::uint32_t type, std::uint64_t elapsed_us) const {
        const std::uint64_t offset = offset_us[type];
        if (elapsed_us <= offset) return 0;
        const std::uint64_t every = every_us(type);
        return (elapsed_us - offset + every - 1) / every;
    }
};

inline std::uint64_t frame_token(std::uint32_t type, std::uint64_t index) {
    const std::uint64_t sequence = index + 1;
    return type == kFrameEventOrigin ? sequence : (std::uint64_t{type} << kFrameTokenTypeShift) | sequence;
}

inline bool frame_interval_valid(std::uint32_t interval_us, std::uint32_t periods) {
    return interval_us != 0 && periods != 0;
}

}  // namespace g6shim::d3d12
