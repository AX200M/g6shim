// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_computer.hpp"

extern "C" HRESULT g6shim_XGCreateTextureComputer(const g6shim::xg_xs::ResourceDesc* desc,
                                                  g6shim::xg_xs::Computer** out) {
    return g6shim::xg_xs::create_texture_computer(desc, out);
}

extern "C" HRESULT g6shim_XGSuggestSwizzleMode(const g6shim::xg_xs::ResourceDesc* desc, std::uint32_t flags,
                                               std::uint32_t* out) {
    return g6shim::xg_xs::suggest_swizzle_mode(desc, flags, out);
}
