// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/window.hpp"

#include <atomic>

namespace g6shim {

namespace {

std::atomic<void*> g_main_window{nullptr};

}  // namespace

void set_main_window(void* hwnd) { g_main_window = hwnd; }

void* main_window() { return g_main_window; }

}  // namespace g6shim
