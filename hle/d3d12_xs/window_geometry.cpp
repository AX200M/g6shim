// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_geometry.hpp"
#include <g6shim/settings.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace g6shim::d3d12_xs::window {

namespace {

constexpr std::size_t kFrameRateChars = 96;

std::string_view trimmed(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::optional<std::int32_t> dimension(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) return std::nullopt;
    std::int32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if (value <= 0 || value > kLargestClientDimension) return std::nullopt;
    return value;
}


}  // namespace

Size fit_into(const Rect& area, std::int32_t width, std::int32_t height) noexcept {
    const std::int32_t area_width = area.right - area.left;
    const std::int32_t area_height = area.bottom - area.top;
    if (width <= 0 || height <= 0 || area_width <= 0 || area_height <= 0 ||
        (width <= area_width && height <= area_height))
        return Size{width, height};
    const double scale = std::min(static_cast<double>(area_width) / static_cast<double>(width),
                                  static_cast<double>(area_height) / static_cast<double>(height));
    return Size{static_cast<std::int32_t>(static_cast<double>(width) * scale),
                static_cast<std::int32_t>(static_cast<double>(height) * scale)};
}

Placement fit(const Rect& area, std::int32_t x, std::int32_t y, std::int32_t width,
              std::int32_t height) noexcept {
    const Size client = fit_into(area, width, height);
    if (client.width == width && client.height == height) return Placement{x, y, client};
    const std::int32_t area_width = area.right - area.left;
    const std::int32_t area_height = area.bottom - area.top;
    return Placement{area.left + (area_width - client.width) / 2,
                     area.top + (area_height - client.height) / 2, client};
}

std::optional<Size> parse_size(std::string_view text) noexcept {
    text = trimmed(text);
    const std::size_t separator = text.find_first_of("xX");
    if (separator == std::string_view::npos ||
        text.find_first_of("xX", separator + 1) != std::string_view::npos)
        return std::nullopt;
    const auto width = dimension(text.substr(0, separator));
    const auto height = dimension(text.substr(separator + 1));
    if (!width || !height) return std::nullopt;
    return Size{*width, *height};
}

std::optional<Size> forced_size() noexcept {
    const std::optional<std::string> text = settings::get("G6SHIM_WINDOW");
    if (!text) return std::nullopt;
    return parse_size(*text);
}

Size requested_size(Size wanted) noexcept {
    const std::optional<Size> forced = forced_size();
    return forced ? *forced : wanted;
}

bool fullscreen_requested() noexcept { return settings::enabled("G6SHIM_FULLSCREEN"); }

Placement fullscreen_placement(const Rect& monitor) noexcept {
    return Placement{monitor.left, monitor.top,
                     Size{monitor.right - monitor.left, monitor.bottom - monitor.top}};
}

std::wstring caption(std::wstring_view base, Size render, Size client, std::optional<FrameRate> rate) {
    std::wstring text(base);
    if (render.width > 0 && render.height > 0) {
        text += L" - " + std::to_wstring(render.width) + L"x" + std::to_wstring(render.height);
        if (client.width > 0 && client.height > 0 && client != render)
            text += L" (window " + std::to_wstring(client.width) + L"x" + std::to_wstring(client.height) + L")";
    }
    if (rate) {
        wchar_t suffix[kFrameRateChars] = {};
        std::swprintf(suffix, kFrameRateChars, L" | g6shim %.0f fps %.1f ms/frame", rate->fps, rate->milliseconds);
        text += suffix;
    }
    return text;
}

}  // namespace g6shim::d3d12_xs::window
