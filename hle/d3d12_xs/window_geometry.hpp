// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace g6shim::d3d12_xs::window {

struct Rect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;

    constexpr bool operator==(const Size&) const = default;
};

struct Placement {
    std::int32_t x = 0;
    std::int32_t y = 0;
    Size client;
};

inline constexpr std::int32_t kUsedDefault = std::numeric_limits<std::int32_t>::min();
inline constexpr std::int32_t kLargestClientDimension = 32767;

Size fit_into(const Rect& area, std::int32_t width, std::int32_t height) noexcept;
Placement fit(const Rect& area, std::int32_t x, std::int32_t y, std::int32_t width,
              std::int32_t height) noexcept;

std::optional<Size> parse_size(std::string_view text) noexcept;
std::optional<Size> forced_size() noexcept;
Size requested_size(Size wanted) noexcept;
bool fullscreen_requested() noexcept;
Placement fullscreen_placement(const Rect& monitor) noexcept;

struct FrameRate {
    double fps = 0.0;
    double milliseconds = 0.0;
};

std::wstring caption(std::wstring_view base, Size render, Size client, std::optional<FrameRate> rate);

}  // namespace g6shim::d3d12_xs::window
