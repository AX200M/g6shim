// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dxgi.h>

namespace g6shim::d3d {

void capture_if_requested(IDXGISwapChain* swap_chain, unsigned frame);

}  // namespace g6shim::d3d
