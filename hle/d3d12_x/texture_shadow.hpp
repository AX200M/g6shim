// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace g6shim::d3d12 {

namespace resource_slot {
inline constexpr std::size_t kRelease = 2;
inline constexpr std::size_t kGetGpuVirtualAddress = 11;
inline constexpr std::size_t kSlots = 17;
}  // namespace resource_slot

inline constexpr UINT64 kNoShadowAddress = 0;

struct ShadowCopy {
    UINT subresource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
};

bool shadow_copies(const D3D12_RESOURCE_DESC& desc, std::vector<ShadowCopy>* copies, UINT64* shadow_bytes);

void track_texture(ID3D12Resource* texture);
void flush_texture_shadows(ID3D12Device* device, ID3D12CommandQueue* queue);

}  // namespace g6shim::d3d12
