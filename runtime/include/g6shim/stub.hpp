// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <atomic>
#include <cstdint>

#define G6SHIM_STUB(id, dll, name)                                            \
    extern "C" std::uintptr_t g6shim_stub_##id() {                            \
        static std::atomic<bool> g6shim_reported{false};                      \
        if (!g6shim_reported.exchange(true, std::memory_order_relaxed))       \
            ::g6shim::log_unimplemented(dll, name);                           \
        return 0;                                                             \
    }
