// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "texture_shadow.hpp"

#include "layout.hpp"

#include <g6shim/log.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace g6shim::d3d12 {

namespace {

constexpr int kLoggedRefusals = 4;

struct PatchedTable {
    std::array<const void*, resource_slot::kSlots> slots;
    const void* const* original;
};

struct Shadow {
    void* memory = nullptr;
    UINT64 bytes = 0;
    std::vector<ShadowCopy> copies;
    ID3D12Resource* upload = nullptr;
    void* upload_memory = nullptr;
};

struct Uploader {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 submitted = 0;
};

struct Registry {
    std::mutex mutex;
    std::map<const void* const*, std::unique_ptr<PatchedTable>> tables;
    std::map<ID3D12Resource*, Shadow> textures;
    std::map<ID3D12CommandQueue*, Uploader> uploaders;
};

Registry& registry() {
    static Registry r;
    return r;
}

const PatchedTable* patched_table(ID3D12Resource* resource) {
    return *reinterpret_cast<const PatchedTable* const*>(resource);
}

void release_shadow(Shadow& shadow) {
    if (shadow.upload) shadow.upload->Release();
    if (shadow.memory) VirtualFree(shadow.memory, 0, MEM_RELEASE);
    shadow = {};
}

ULONG STDMETHODCALLTYPE texture_release(ID3D12Resource* self) {
    using Release = ULONG(STDMETHODCALLTYPE*)(ID3D12Resource*);
    const auto original = reinterpret_cast<Release>(patched_table(self)->original[resource_slot::kRelease]);
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const ULONG left = original(self);
    if (left == 0) {
        const auto it = r.textures.find(self);
        if (it != r.textures.end()) {
            release_shadow(it->second);
            r.textures.erase(it);
        }
    }
    return left;
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE texture_gpu_address(ID3D12Resource* self) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    Shadow& shadow = r.textures[self];
    if (shadow.memory) return reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(shadow.memory);
    const D3D12_RESOURCE_DESC desc = self->GetDesc();
    std::vector<ShadowCopy> copies;
    UINT64 bytes = 0;
    static std::atomic<int> refused{0};
    if (!shadow_copies(desc, &copies, &bytes)) {
        if (refused.fetch_add(1, std::memory_order_relaxed) < kLoggedRefusals)
            G6SHIM_WARN("texture CPU address: dimension %d, format %d, %llux%u has no linear layout -> 0",
                        desc.Dimension, desc.Format, desc.Width, desc.Height);
        return kNoShadowAddress;
    }
    shadow.memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!shadow.memory) {
        G6SHIM_WARN("texture CPU address: %llu bytes of shadow memory not available -> 0", bytes);
        return kNoShadowAddress;
    }
    shadow.bytes = bytes;
    shadow.copies = std::move(copies);
    return reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(shadow.memory);
}

const void* const* patch_table(const void* const* original) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto& table = r.tables[original];
    if (!table) {
        table = std::make_unique<PatchedTable>();
        std::memcpy(table->slots.data(), original, sizeof table->slots);
        table->slots[resource_slot::kRelease] = reinterpret_cast<const void*>(&texture_release);
        table->slots[resource_slot::kGetGpuVirtualAddress] = reinterpret_cast<const void*>(&texture_gpu_address);
        table->original = original;
    }
    return table->slots.data();
}

bool written(const Shadow& shadow) {
    void* page = nullptr;
    ULONG_PTR pages = 1;
    ULONG granularity = 0;
    return GetWriteWatch(0, shadow.memory, shadow.bytes, &page, &pages, &granularity) == 0 && pages > 0;
}

bool ensure_upload(ID3D12Device* device, Shadow& shadow) {
    if (shadow.upload) return true;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = shadow.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&shadow.upload))))
        return false;
    if (FAILED(shadow.upload->Map(0, nullptr, &shadow.upload_memory))) {
        shadow.upload->Release();
        shadow.upload = nullptr;
        return false;
    }
    return true;
}

bool open_uploader(ID3D12Device* device, ID3D12CommandQueue* queue, Uploader& uploader) {
    if (!uploader.fence) {
        const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
        if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&uploader.allocator))) ||
            FAILED(device->CreateCommandList(0, type, uploader.allocator, nullptr, IID_PPV_ARGS(&uploader.list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploader.fence))))
            return false;
        return true;
    }
    if (uploader.fence->GetCompletedValue() < uploader.submitted)
        uploader.fence->SetEventOnCompletion(uploader.submitted, nullptr);
    return SUCCEEDED(uploader.allocator->Reset()) && SUCCEEDED(uploader.list->Reset(uploader.allocator, nullptr));
}

void record_upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, Shadow& shadow) {
    ResetWriteWatch(shadow.memory, shadow.bytes);
    std::memcpy(shadow.upload_memory, shadow.memory, shadow.bytes);
    for (const ShadowCopy& copy : shadow.copies) {
        D3D12_TEXTURE_COPY_LOCATION destination{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        destination.SubresourceIndex = copy.subresource;
        D3D12_TEXTURE_COPY_LOCATION source{shadow.upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        source.PlacedFootprint = copy.footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
}

}  // namespace

bool shadow_copies(const D3D12_RESOURCE_DESC& desc, std::vector<ShadowCopy>* copies, UINT64* shadow_bytes) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width > UINT32_MAX) return false;
    xg::Texture2DDesc texture{};
    texture.width = static_cast<std::uint32_t>(desc.Width);
    texture.height = desc.Height;
    texture.mip_levels = desc.MipLevels;
    texture.array_size = desc.DepthOrArraySize;
    texture.format = static_cast<std::uint32_t>(desc.Format);
    texture.sample_count = desc.SampleDesc.Count;
    xg::FormatInfo format{};
    auto layout = std::make_unique<xg::ResourceLayout>();
    if (texture.sample_count > 1 || !xg::format_info(texture.format, &format) ||
        FAILED(xg::compute_linear_layout(texture, layout.get())))
        return false;
    const UINT slices = desc.DepthOrArraySize;
    copies->clear();
    copies->reserve(std::size_t{layout->mip_levels} * slices);
    for (UINT slice = 0; slice < slices; ++slice) {
        for (UINT level = 0; level < layout->mip_levels; ++level) {
            const xg::MipLayout& mip = layout->plane[0].mips[level];
            ShadowCopy& copy = copies->emplace_back();
            copy.subresource = level + slice * layout->mip_levels;
            copy.footprint.Offset = mip.offset_bytes + UINT64{slice} * mip.slice_2d_size_bytes;
            copy.footprint.Footprint.Format = desc.Format;
            copy.footprint.Footprint.Width = mip.width_elements * format.block_width;
            copy.footprint.Footprint.Height = mip.height_elements * format.block_height;
            copy.footprint.Footprint.Depth = 1;
            copy.footprint.Footprint.RowPitch = mip.pitch_bytes;
        }
    }
    *shadow_bytes = layout->size_bytes;
    return true;
}

void track_texture(ID3D12Resource* texture) {
    if (!texture) return;
    auto** table = reinterpret_cast<const void* const**>(texture);
    *table = patch_table(*table);
}

void flush_texture_shadows(ID3D12Device* device, ID3D12CommandQueue* queue) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    bool opened = false;
    Uploader& uploader = r.uploaders[queue];
    for (auto& [texture, shadow] : r.textures) {
        if (!shadow.memory || !written(shadow)) continue;
        if (!opened && !open_uploader(device, queue, uploader)) {
            G6SHIM_WARN("texture shadow upload: no command list for the queue");
            return;
        }
        opened = true;
        if (!ensure_upload(device, shadow)) {
            G6SHIM_WARN("texture shadow upload: no upload buffer of %llu bytes", shadow.bytes);
            continue;
        }
        record_upload(uploader.list, texture, shadow);
    }
    if (!opened) return;
    uploader.list->Close();
    ID3D12CommandList* lists[] = {uploader.list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(uploader.fence, ++uploader.submitted);
}

}  // namespace g6shim::d3d12
