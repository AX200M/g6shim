// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "window_geometry.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::d3d12_xs::window {

using SuspendRoutine = void (*)(std::uint8_t quiesced, void* context);

inline constexpr std::uint32_t kInvalidParameter = 87;
inline constexpr std::uint32_t kSuspendDeadlineMilliseconds = 1000;

std::uint32_t register_app_state_change(SuspendRoutine routine, void* context, std::size_t* registration);
void unregister_app_state_change(std::size_t registration);

void end_title(const char* why);

HWND create(std::uint32_t width, std::uint32_t height);
void adopt(HWND hwnd);
void show_render_size(HWND hwnd, std::uint32_t width, std::uint32_t height);
void show_frame_rate(HWND hwnd, FrameRate rate);

}  // namespace g6shim::d3d12_xs::window
