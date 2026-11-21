// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

namespace g6shim {

G6SHIM_RUNTIME_API void set_main_window(void* hwnd);
G6SHIM_RUNTIME_API void* main_window();

}  // namespace g6shim
