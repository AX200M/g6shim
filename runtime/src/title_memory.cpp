// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/title_memory.hpp"

#include <atomic>

namespace g6shim {

namespace {

std::atomic<TitleMemoryRemapObserver> g_observer{nullptr};

}  // namespace

void set_title_memory_remap_observer(TitleMemoryRemapObserver observer) {
    g_observer.store(observer, std::memory_order_release);
}

void title_memory_remapped(const void* at, std::size_t bytes) {
    const TitleMemoryRemapObserver observer = g_observer.load(std::memory_order_acquire);
    if (observer) observer(at, bytes);
}

}  // namespace g6shim
