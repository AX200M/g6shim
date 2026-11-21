// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "constant_views.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace g6shim::d3d12 {

namespace {

struct UploadBuffer {
    UINT64 end;
    const unsigned char* cpu;
};

struct Relocation {
    const unsigned char* source;
    UINT64 offset;
    UINT size;
};

struct Relocations {
    std::mutex mutex;
    std::map<UINT64, UploadBuffer> uploads;
    std::map<std::pair<UINT64, UINT>, Relocation> relocations;
    ID3D12Resource* buffer = nullptr;
    unsigned char* buffer_cpu = nullptr;
    UINT64 buffer_gpu = 0;
    UINT64 used = 0;
};

Relocations& relocations() {
    static Relocations* r = new Relocations;
    return *r;
}

const unsigned char* source_cpu(Relocations& r, UINT64 location, UINT size) {
    auto it = r.uploads.upper_bound(location);
    if (it == r.uploads.begin()) return nullptr;
    --it;
    if (location + size > it->second.end) return nullptr;
    return it->second.cpu + (location - it->first);
}

bool ensure_buffer(ID3D12Device* device, Relocations& r) {
    if (r.buffer) return true;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kRelocatedConstantBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* cpu = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&r.buffer))))
        return false;
    if (FAILED(r.buffer->Map(0, nullptr, &cpu))) {
        r.buffer->Release();
        r.buffer = nullptr;
        return false;
    }
    r.buffer_cpu = static_cast<unsigned char*>(cpu);
    r.buffer_gpu = r.buffer->GetGPUVirtualAddress();
    return true;
}

}  // namespace

void track_upload_buffer(ID3D12Resource* buffer, UINT64 bytes) {
    void* cpu = nullptr;
    if (!buffer || FAILED(buffer->Map(0, nullptr, &cpu))) return;
    Relocations& r = relocations();
    std::lock_guard<std::mutex> lock(r.mutex);
    const UINT64 start = buffer->GetGPUVirtualAddress();
    r.uploads[start] = {start + bytes, static_cast<const unsigned char*>(cpu)};
}

bool relocate_constants(ID3D12Device* device, UINT64 location, UINT size, UINT64* pc_location) {
    Relocations& r = relocations();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto key = std::make_pair(location, size);
    if (const auto it = r.relocations.find(key); it != r.relocations.end()) {
        *pc_location = r.buffer_gpu + it->second.offset;
        return true;
    }
    const unsigned char* source = source_cpu(r, location, size);
    const UINT slot = pc_constant_view_size(size);
    if (!source || !ensure_buffer(device, r) || r.used + slot > kRelocatedConstantBytes) return false;
    const Relocation relocation{source, r.used, size};
    r.used += slot;
    std::memcpy(r.buffer_cpu + relocation.offset, source, size);
    r.relocations.emplace(key, relocation);
    *pc_location = r.buffer_gpu + relocation.offset;
    return true;
}

const void* relocated_constants(UINT64 pc_location) {
    Relocations& r = relocations();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.buffer || pc_location < r.buffer_gpu || pc_location >= r.buffer_gpu + kRelocatedConstantBytes) return nullptr;
    return r.buffer_cpu + (pc_location - r.buffer_gpu);
}

void refresh_relocated_constants() {
    Relocations& r = relocations();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto& [key, relocation] : r.relocations)
        std::memcpy(r.buffer_cpu + relocation.offset, relocation.source, relocation.size);
}

}  // namespace g6shim::d3d12
