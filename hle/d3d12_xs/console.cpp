// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console.hpp"

#include <atomic>

namespace g6shim::d3d12_xs {

namespace {

std::atomic<bool> g_series_s{false};

}  // namespace

void set_console(Console console) {
    g_series_s.store(console == Console::kSeriesS, std::memory_order_relaxed);
}

Console current_console() {
    return g_series_s.load(std::memory_order_relaxed) ? Console::kSeriesS : Console::kSeriesX;
}

}  // namespace g6shim::d3d12_xs
