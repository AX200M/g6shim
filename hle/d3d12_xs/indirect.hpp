// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "host.hpp"

#include <d3d12.h>

#include <cstdint>

namespace g6shim::d3d12_xs::indirect {

bool clamp_available();
bool bound(host::HostList& list, ID3D12Resource* buffer, std::uint64_t gpu, std::uint32_t kind,
           std::uint32_t stride, std::uint32_t records);
void done(host::HostList& list, ID3D12Resource* buffer);

}  // namespace g6shim::d3d12_xs::indirect
