// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/indirect.hpp"

#include <windows.h>

namespace {

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

}  // namespace

TEST(d3d12_xs_argument_clamp_builds) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    if (g6shim::d3d12_xs::host::host() == nullptr) return;
    CHECK(g6shim::d3d12_xs::indirect::clamp_available());
}
