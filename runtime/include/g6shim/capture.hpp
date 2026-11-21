// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <cstddef>

namespace g6shim {

enum class PixelOrder { Rgba, Bgra };

G6SHIM_RUNTIME_API bool capture_requested(unsigned frame);
G6SHIM_RUNTIME_API bool save_captured_frame(unsigned frame, const void* pixels, unsigned width, unsigned height,
                                            std::size_t row_pitch, PixelOrder order);

}  // namespace g6shim
