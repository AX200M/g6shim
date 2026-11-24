// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console_xg.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <cstdint>

namespace {

using Arguments8 = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                      std::uintptr_t, std::uintptr_t, std::uintptr_t);

constexpr std::uintptr_t kNotImplemented = static_cast<std::uintptr_t>(static_cast<std::uint32_t>(E_NOTIMPL));

std::uintptr_t forward(const char* name, std::atomic<bool>& reported, std::uintptr_t a, std::uintptr_t b,
                       std::uintptr_t c, std::uintptr_t d, std::uintptr_t e, std::uintptr_t f, std::uintptr_t g,
                       std::uintptr_t h) {
    if (const auto console = reinterpret_cast<Arguments8>(g6shim::xg::console_export(name)))
        return console(a, b, c, d, e, f, g, h);
    if (!reported.exchange(true, std::memory_order_relaxed)) g6shim::log_unimplemented("xg_x.dll", name);
    return kNotImplemented;
}

}  // namespace

#define G6SHIM_XG_FORWARD(name)                                                                                    \
    extern "C" std::uintptr_t g6shim_##name(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d, \
                                            std::uintptr_t e, std::uintptr_t f, std::uintptr_t g, std::uintptr_t h) { \
        static std::atomic<bool> reported{false};                                                                  \
        return forward(#name, reported, a, b, c, d, e, f, g, h);                                                   \
    }

G6SHIM_XG_FORWARD(XGComputeOptimalDepthStencilTileModes)
G6SHIM_XG_FORWARD(XGComputeTexture2DLayout)
G6SHIM_XG_FORWARD(XGCreateTextureComputer)
