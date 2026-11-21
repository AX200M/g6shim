// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <d3d12.h>

namespace g6shim::d3d12 {

void enable_removal_diagnostics();
bool report_device_removal(ID3D12Device* device);

}  // namespace g6shim::d3d12
