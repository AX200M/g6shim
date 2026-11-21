// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <d3d11.h>

namespace g6shim::d3d {

void hook_dxgi_factory(ID3D11Device* device);

}  // namespace g6shim::d3d
