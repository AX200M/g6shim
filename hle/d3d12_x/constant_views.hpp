// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>

namespace g6shim::d3d12 {

inline constexpr UINT kConstantViewAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
inline constexpr UINT64 kRelocatedConstantBytes = 4u << 20;

inline bool constant_view_fits_pc(UINT64 location, UINT size) {
    return location % kConstantViewAlignment == 0 && size % kConstantViewAlignment == 0;
}

inline UINT pc_constant_view_size(UINT size) {
    return (size + kConstantViewAlignment - 1) / kConstantViewAlignment * kConstantViewAlignment;
}

void track_upload_buffer(ID3D12Resource* buffer, UINT64 bytes);
bool relocate_constants(ID3D12Device* device, UINT64 location, UINT size, UINT64* pc_location);
const void* relocated_constants(UINT64 pc_location);
void refresh_relocated_constants();

}  // namespace g6shim::d3d12
