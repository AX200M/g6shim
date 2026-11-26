// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx10_layout.hpp"
#include "host.hpp"
#include "resource.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::later {

struct DepthUavPlane {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    std::atomic<std::uint32_t> state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    std::uint32_t format = 0;
    float clear_float = 0;
    std::uint32_t clear_uint = 0;
};

struct DepthUav {
    bool direct = false;
    DepthUavPlane depth;
    DepthUavPlane stencil;
};

struct HostTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    std::atomic<std::uint32_t> state{0};
    std::shared_ptr<DepthUav> depth_uav;
};

}  // namespace g6shim::d3d12_xs::later

namespace g6shim::d3d12_xs::texture {

struct Last {
    std::optional<std::uint64_t> hash;
    std::uint64_t asked = 0;
    std::vector<std::uint8_t> read;
};

class Staging {
public:
    Staging(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::uint8_t* data,
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints, std::vector<std::uint32_t> rows,
            gfx10::Layout layout, std::uint32_t mips, const std::uint8_t* memory);

    void refresh();

    ID3D12Resource* buffer() const { return buffer_.Get(); }
    std::uint8_t* data() const { return data_; }
    const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& footprints() const { return footprints_; }
    const std::vector<std::uint32_t>& rows() const { return rows_; }
    const gfx10::Layout& layout() const { return layout_; }
    const std::uint8_t* memory() const { return memory_; }
    std::size_t memory_bytes() const { return static_cast<std::size_t>(layout_.size); }

private:
    std::uint64_t refresh_now();
    void detile(std::span<const std::uint8_t> tiled);

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::uint8_t* data_ = nullptr;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints_;
    std::vector<std::uint32_t> rows_;
    gfx10::Layout layout_;
    std::uint32_t mips_ = 0;
    const std::uint8_t* memory_ = nullptr;
    std::mutex last_mutex_;
    Last last_;
};

std::shared_ptr<later::HostTexture> create_host_texture(const resource::ResourceDesc& desc, std::uint32_t state,
                                                        const void* clear);
ID3D12Resource* host_texture(Object* resource);
std::optional<std::uint32_t> host_texture_format(Object* resource);
std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* resource, std::uint32_t wanted);

void forget(Object* resource);
void host_written(Object* resource);
bool committed(const void* at, std::size_t bytes);
bool title_written(ID3D12Resource* texture);
void upload(host::HostList& list, Object* resource, ID3D12Resource* texture);
std::optional<std::shared_ptr<Staging>> staging(Object* resource, ID3D12Resource* texture);

struct DepthUavBinding {
    ID3D12Resource* resource = nullptr;
    std::uint32_t format = 0;
    std::uint32_t plane = 0;
    bool twin = false;
};

struct GpuRange {
    std::uint64_t gpu = 0;
    std::uint64_t bytes = 0;
    std::string name;
};

std::vector<GpuRange> live_ranges();
std::vector<GpuRange> live_textures();
bool depth_unordered_direct(std::uint32_t format, std::uint32_t samples);
void attach_depth_uav(later::HostTexture& host, const resource::ResourceDesc& host_desc, bool direct,
                      const void* clear);
std::optional<DepthUavBinding> depth_uav_binding(Object* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc);
void note_depth_uav(host::HostList& list, Object* resource, const std::uint32_t* words);
bool copy_depth_uavs(host::HostList& list);
void drop_depth_uav_notes(host::HostList* list);

}  // namespace g6shim::d3d12_xs::texture
