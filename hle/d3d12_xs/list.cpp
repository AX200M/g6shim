// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "list.hpp"

#include "descriptor.hpp"
#include "device.hpp"
#include "pipeline_bridge.hpp"
#include "resource.hpp"
#include "signature.hpp"
#include "texture.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace g6shim::d3d12_xs::list {

namespace {

constexpr std::uint32_t kBeginOnly = 1;
constexpr std::uint32_t kAllSubresources = 0xffff'ffffu;
constexpr std::uint32_t kCopyDest = 0x400;
constexpr std::uint32_t kCopySource = 0x800;
constexpr std::uint32_t kResolveDest = 0x1000;
constexpr std::uint32_t kResolveSource = 0x2000;
constexpr std::uint32_t kRenderTarget = 0x4;
constexpr std::uint32_t kDepthWrite = 0x10;
constexpr std::uint32_t kFootprintLocation = 1;

struct Allocator : Object {
    Allocator(const void* const* functions, std::uint32_t allocator_kind)
        : Object(functions), kind(allocator_kind) {}

    std::uint32_t kind;
};

struct TitleListLayout {
    Object object;
    std::uint32_t kind;
    std::array<std::uint64_t, 5> put;
    std::array<std::uint32_t, 64> reserved;
    std::array<std::uint32_t, 128> graphics_root_headers;
    std::array<std::uint32_t, 128> compute_root_headers;
    std::array<std::uint32_t, 8> pipeline_shadow;
    std::array<std::uint32_t, 256> newer;
};
static_assert(offsetof(TitleListLayout, put) == 0x18);
static_assert(offsetof(TitleListLayout, pipeline_shadow) + 32 == 0x560);

struct List : Object {
    List(const void* const* functions, std::uint32_t list_kind) : Object(functions), kind(list_kind) {}

    std::uint32_t kind;
    std::array<std::uint64_t, 5> put{};
    std::array<std::uint32_t, 64> reserved{};
    std::array<std::uint32_t, 128> graphics_root_headers{};
    std::array<std::uint32_t, 128> compute_root_headers{};
    std::array<std::uint32_t, 8> pipeline_shadow{};
    std::array<std::uint32_t, 256> newer{};
    std::mutex uploads_mutex;
    std::vector<std::vector<std::uint8_t>> uploads;
    std::unique_ptr<host::HostList> host;
    std::mutex state_mutex;
    draw::State state;
};

std::atomic<std::uint64_t> g_draws{0};
std::atomic<std::uint64_t> g_dispatches{0};
std::atomic<std::uint64_t> g_indirect{0};

void count_draw() { g_draws.fetch_add(1, std::memory_order_relaxed); }

void count_dispatch() { g_dispatches.fetch_add(1, std::memory_order_relaxed); }

template <typename T>
std::span<const T> items(const T* data, std::uint32_t count) {
    if (data == nullptr || count == 0) return {};
    return std::span<const T>(data, count);
}

List* title_list(Object* object) { return static_cast<List*>(object); }

template <typename F>
void with_state(Object* object, F&& work) {
    host::HostList* host = host_list(object);
    if (host == nullptr) return;
    List* list_object = title_list(object);
    std::lock_guard<std::mutex> lock(list_object->state_mutex);
    work(*host, list_object->state);
}

std::optional<std::uint8_t*> buffer_range(Object* buffer, std::uint64_t offset, std::uint64_t length) {
    const auto bytes = resource::buffer_bytes(buffer);
    if (!bytes) return std::nullopt;
    if (offset > bytes->second || length > bytes->second - offset) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a copy lies outside its buffer");
        });
        return std::nullopt;
    }
    return bytes->first + offset;
}

D3D12_RESOURCE_BARRIER uav_barrier() {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = nullptr;
    return barrier;
}

std::optional<Barrier> host_barrier(host::HostList& list, const Barrier& barrier) {
    const auto host_resource = [](const void* resource) -> std::optional<const void*> {
        if (resource == nullptr) return resource;
        ID3D12Resource* texture = texture::host_texture(static_cast<Object*>(const_cast<void*>(resource)));
        if (texture == nullptr) return std::nullopt;
        return texture;
    };
    switch (barrier.kind) {
    case 0: {
        if ((barrier.flags & kBeginOnly) != 0) return std::nullopt;
        const std::uint32_t wanted = host::texture_state(static_cast<std::uint32_t>(barrier.last));
        const auto changed =
            list.transition(static_cast<Object*>(const_cast<void*>(barrier.first)), wanted);
        if (!changed) return std::nullopt;
        return Barrier{0,
                       0,
                       changed->first,
                       static_cast<std::uint64_t>(kAllSubresources) |
                           (static_cast<std::uint64_t>(changed->second) << 32),
                       static_cast<std::uint64_t>(host::legal_state(changed->first, wanted))};
    }
    case 1: {
        const auto first = host_resource(barrier.first);
        if (!first) return std::nullopt;
        std::uint64_t second = 0;
        if (barrier.second != 0) {
            const auto other = host_resource(reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(barrier.second)));
            if (!other) return std::nullopt;
            second = reinterpret_cast<std::uintptr_t>(*other);
        }
        return Barrier{1, barrier.flags, *first, second, barrier.last};
    }
    case 2: {
        const auto first = host_resource(barrier.first);
        if (!first) return std::nullopt;
        return Barrier{2, barrier.flags, *first, barrier.second, barrier.last};
    }
    default: return std::nullopt;
    }
}

}  // namespace

extern "C" {

HRESULT allocator_query(Object* object, const Iid* iid, Object** out) {
    return answer(kAllocator, iid, is_iid(iid, kAllocatorIids) ? object : nullptr, out);
}

std::uint32_t allocator_release(Object* object) { return release_boxed<Allocator>(object); }

HRESULT allocator_reset(Object*) { return S_OK; }

HRESULT list_query(Object* object, const Iid* iid, Object** out) {
    return answer(kList, iid, is_iid(iid, kListIids) ? object : nullptr, out);
}

std::uint32_t list_release(Object* object) { return release_boxed<List>(object); }

std::uint32_t list_type(Object* object) { return title_list(object)->kind; }

HRESULT list_close(Object* object) {
    if (host::HostList* host = host_list(object)) host->close();
    return S_OK;
}

HRESULT list_reset(Object* object, Object* allocator, Object* pipeline) {
    if (allocator == nullptr) return E_INVALIDARG;
    {
        List* list_object = title_list(object);
        std::lock_guard<std::mutex> lock(list_object->uploads_mutex);
        list_object->uploads.clear();
    }
    with_state(object, [pipeline](host::HostList& host, draw::State& state) {
        host.reset();
        state.reset(pipeline);
    });
    return S_OK;
}

std::uint32_t barrier(Object* object, std::uint32_t count, const Barrier* barriers) {
    if (host::HostList* host = host_list(object); host != nullptr && barriers != nullptr) {
        std::vector<Barrier> converted;
        for (const Barrier& entry : std::span<const Barrier>(barriers, count)) {
            const auto host_entry = host_barrier(*host, entry);
            if (host_entry) converted.push_back(*host_entry);
        }
        for (const Barrier& entry : std::span<const Barrier>(barriers, count)) {
            if (entry.kind != 1 && entry.first != nullptr &&
                resource::buffer_bytes(static_cast<Object*>(const_cast<void*>(entry.first))))
                converted.push_back(Barrier{2, 0, nullptr, 0, 0});
        }
        if (!converted.empty()) {
            std::vector<D3D12_RESOURCE_BARRIER> native(converted.size());
            std::memcpy(native.data(), converted.data(), native.size() * sizeof(D3D12_RESOURCE_BARRIER));
            host->list()->ResourceBarrier(static_cast<UINT>(native.size()), native.data());
        }
    }
    return 0;
}

void copy_buffer_region(Object* object, Object* destination, std::uint64_t destination_offset, Object* source,
                        std::uint64_t source_offset, std::uint64_t length) {
    host::HostList* host = host_list(object);
    if (host == nullptr) return;
    const auto to = buffer_range(destination, destination_offset, length);
    const auto from = buffer_range(source, source_offset, length);
    if (!to || !from) return;
    host->defer_copy(*to, *from, static_cast<std::size_t>(length));
}

void copy_resource(Object* object, Object* destination, Object* source) {
    host::HostList* host = host_list(object);
    if (host == nullptr) return;
    ID3D12Resource* to = texture::host_texture(destination);
    ID3D12Resource* from = texture::host_texture(source);
    if (to != nullptr && from != nullptr) {
        if (texture::title_written(from)) texture::upload(*host, source, from);
        host::ensure_state(*host, destination, kCopyDest);
        host::ensure_state(*host, source, kCopySource);
        texture::host_written(destination);
        host->list()->CopyResource(to, from);
        return;
    }
    const auto destination_bytes = resource::buffer_bytes(destination);
    const auto source_bytes = resource::buffer_bytes(source);
    if (!destination_bytes || !source_bytes) return;
    host->defer_copy(destination_bytes->first, source_bytes->first,
                     static_cast<std::size_t>(std::min(destination_bytes->second, source_bytes->second)));
}

void copy_resource_x(Object* object, Object* destination, Object* source, std::uint32_t) {
    copy_resource(object, destination, source);
}

void copy_texture_region(Object* object, const CopyLocation* destination, std::uint32_t x, std::uint32_t y,
                         std::uint32_t z, const CopyLocation* source, const std::uint32_t* source_box) {
    host::HostList* host = host_list(object);
    if (host == nullptr || destination == nullptr || source == nullptr) return;
    ID3D12Resource* to = texture::host_texture(destination->resource);
    if (to == nullptr) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: texture to buffer copies not implemented");
        });
        return;
    }
    CopyLocation from = *source;
    ID3D12Resource* origin_resource = nullptr;
    if (source->kind == kFootprintLocation) {
        host::Host* gpu = host::host();
        const auto bytes = resource::footprint_bytes(source->footprint);
        if (gpu == nullptr || !bytes) return;
        const auto data = buffer_range(source->resource, source->footprint.offset, *bytes);
        if (!data) return;
        const auto staging = host->upload_with_resource(*bytes);
        if (!staging) return;
        host->defer_fill(staging->data, *data, static_cast<std::size_t>(*bytes));
        origin_resource = staging->buffer.Get();
        from.footprint.offset = 0;
    } else {
        ID3D12Resource* texture = texture::host_texture(source->resource);
        if (texture == nullptr) return;
        if (texture::title_written(texture)) texture::upload(*host, source->resource, texture);
        host::ensure_state(*host, source->resource, kCopySource);
        origin_resource = texture;
    }
    host::ensure_state(*host, destination->resource, kCopyDest);
    texture::host_written(destination->resource);
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = to;
    location.Type = static_cast<D3D12_TEXTURE_COPY_TYPE>(destination->kind);
    if (destination->kind == kFootprintLocation)
        std::memcpy(&location.PlacedFootprint, &destination->footprint, sizeof location.PlacedFootprint);
    else
        location.SubresourceIndex = static_cast<UINT>(destination->footprint.offset);
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = origin_resource;
    origin.Type = static_cast<D3D12_TEXTURE_COPY_TYPE>(from.kind);
    if (from.kind == kFootprintLocation)
        std::memcpy(&origin.PlacedFootprint, &from.footprint, sizeof origin.PlacedFootprint);
    else
        origin.SubresourceIndex = static_cast<UINT>(from.footprint.offset);
    const auto* box = source_box != nullptr ? reinterpret_cast<const D3D12_BOX*>(source_box) : nullptr;
    host->list()->CopyTextureRegion(&location, x, y, z, &origin, box);
}

void copy_texture_region_x(Object* object, const CopyLocation* destination, std::uint32_t x, std::uint32_t y,
                           std::uint32_t z, const CopyLocation* source, const std::uint32_t* source_box,
                           std::uint32_t) {
    copy_texture_region(object, destination, x, y, z, source, source_box);
}

void resolve(Object* object, Object* destination, std::uint32_t destination_subresource, Object* source,
             std::uint32_t source_subresource, std::uint32_t format) {
    host::HostList* host = host_list(object);
    if (host == nullptr) return;
    ID3D12Resource* to = texture::host_texture(destination);
    ID3D12Resource* from = texture::host_texture(source);
    if (to == nullptr || from == nullptr) return;
    host::ensure_state(*host, destination, kResolveDest);
    host::ensure_state(*host, source, kResolveSource);
    texture::host_written(destination);
    const D3D12_RESOURCE_DESC destination_desc = to->GetDesc();
    const D3D12_RESOURCE_DESC source_desc = from->GetDesc();
    const auto host_format = static_cast<DXGI_FORMAT>(format);
    if (destination_desc.Width == source_desc.Width && destination_desc.Height == source_desc.Height) {
        host->list()->ResolveSubresource(to, destination_subresource, from, source_subresource, host_format);
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> list1;
    if (FAILED(host->list()->QueryInterface(IID_PPV_ARGS(&list1)))) return;
    const RECT rect{0,
                    0,
                    static_cast<LONG>(std::min(destination_desc.Width, source_desc.Width)),
                    static_cast<LONG>(std::min<UINT>(destination_desc.Height, source_desc.Height))};
    D3D12_RECT region{};
    std::memcpy(&region, &rect, sizeof region);
    list1->ResolveSubresourceRegion(to, destination_subresource, 0, 0, from, source_subresource, &region,
                                    host_format, D3D12_RESOLVE_MODE_AVERAGE);
}

void clear_rtv(Object* object, std::uint64_t handle, const float* color, std::uint32_t count, const RECT* rects) {
    host::HostList* host = host_list(object);
    if (host == nullptr || color == nullptr) return;
    const auto record = descriptor::rtv_record(handle);
    if (!record) return;
    ID3D12Resource* texture = texture::host_texture(record->resource);
    host::Host* gpu = host::host();
    if (texture == nullptr || gpu == nullptr) return;
    const auto description = descriptor::rtv_description(*record);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu->rtv(texture, description ? &*description : nullptr);
    const auto changed = host->transition(record->resource, kRenderTarget);
    if (changed) {
        const D3D12_RESOURCE_BARRIER transition =
            host::transition(changed->first, static_cast<D3D12_RESOURCE_STATES>(changed->second),
                             static_cast<D3D12_RESOURCE_STATES>(kRenderTarget));
        host->list()->ResourceBarrier(1, &transition);
    }
    host->list()->ClearRenderTargetView(rtv, color, count, rects);
}

void clear_dsv(Object* object, std::uint64_t handle, std::uint32_t flags, float depth, std::uint8_t stencil,
               std::uint32_t count, const RECT* rects) {
    host::HostList* host = host_list(object);
    if (host == nullptr) return;
    const auto record = descriptor::dsv_record(handle);
    if (!record) return;
    ID3D12Resource* texture = texture::host_texture(record->resource);
    host::Host* gpu = host::host();
    if (texture == nullptr || gpu == nullptr) return;
    host::ensure_state(*host, record->resource, kDepthWrite);
    const auto description = descriptor::dsv_description(*record);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = gpu->dsv(texture, description ? &*description : nullptr);
    host->list()->ClearDepthStencilView(dsv, static_cast<D3D12_CLEAR_FLAGS>(flags & 3), depth, stencil, count,
                                        rects);
}

void copy_memory(Object* object, std::uint64_t destination, std::uint64_t source, std::uint64_t length,
                 std::uint32_t) {
    host::HostList* host = host_list(object);
    if (host == nullptr || destination == 0 || source == 0) return;
    const std::size_t bytes = static_cast<std::size_t>(length);
    host->defer([destination, source, bytes] {
        if (texture::committed(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(source)), bytes) &&
            texture::committed(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(destination)), bytes))
            std::memmove(reinterpret_cast<void*>(static_cast<std::uintptr_t>(destination)),
                         reinterpret_cast<const void*>(static_cast<std::uintptr_t>(source)), bytes);
    });
}

std::uint64_t driver_hint(Object*, std::uint64_t, std::uint64_t) { return 0; }

void dispatch_x(Object* object, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t) {
    count_dispatch();
    with_state(object, [x, y, z](host::HostList& host, draw::State& state) { state.dispatch(host, x, y, z); });
}

void query_mark(Object*, Object*, std::uint32_t, std::uint32_t) {}

void defer_write(Object* object, std::uint64_t address, host::Written written, std::size_t bytes) {
    if (address == 0) return;
    if (host::HostList* host = host_list(object)) {
        host->defer_write(reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address)), written, bytes);
        return;
    }
    written.write(reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address)), bytes);
}

void resolve_queries(Object* object, Object*, std::uint32_t kind, std::uint32_t, std::uint32_t count,
                     Object* buffer, std::uint64_t offset) {
    constexpr std::uint32_t kOcclusion = 0;
    constexpr std::uint32_t kBinary = 1;
    constexpr std::uint32_t kTimestamp = 2;
    constexpr std::uint32_t kPipelineStatistics = 3;
    const auto bytes = resource::buffer_bytes(buffer);
    if (!bytes) return;
    host::Written written = host::Written::of(0);
    std::uint64_t width = 16;
    switch (kind) {
    case kOcclusion:
        written = host::Written::of(1 << 20);
        width = 8;
        break;
    case kBinary:
        written = host::Written::of(1);
        width = 8;
        break;
    case kTimestamp:
        written = host::Written::gpu_timestamp();
        width = 8;
        break;
    case kPipelineStatistics:
        written = host::Written::of(0);
        width = 88;
        break;
    default: break;
    }
    if (offset > bytes->second || count > (bytes->second - offset) / width) return;
    for (std::uint64_t index = 0; index < count; ++index) {
        for (std::uint64_t piece = 0; piece < width / 8; ++piece) {
            std::uint8_t* at = bytes->first + offset + index * width + piece * 8;
            defer_write(object, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at)),
                        piece == 0 ? written : host::Written::of(0), 8);
        }
    }
}

void set_pipeline(Object* object, Object* pipeline) {
    pipeline_bridge::bound(pipeline);
    with_state(object, [pipeline](host::HostList&, draw::State& state) { state.set_pipeline(pipeline); });
}

void sync(Object*, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t) {}

HRESULT reset_x(Object* object, Object* allocator, Object* pipeline, std::uint32_t) {
    return list_reset(object, allocator, pipeline);
}

void ignored_state() {}

}

void* allocate_upload(Object* object, std::uint64_t size, std::uint64_t alignment) {
    const std::size_t align = static_cast<std::size_t>(alignment > 16 ? alignment : 16);
    if (object == nullptr || !std::has_single_bit(align)) return nullptr;
    if (size > std::numeric_limits<std::size_t>::max() - align) return nullptr;
    const std::size_t length = static_cast<std::size_t>(size) + align;
    std::vector<std::uint8_t> block(length, 0);
    const auto base = reinterpret_cast<std::uintptr_t>(block.data());
    const std::size_t offset = (align - base % align) % align;
    std::uint8_t* at = block.data() + offset;
    List* list_object = title_list(object);
    std::lock_guard<std::mutex> lock(list_object->uploads_mutex);
    list_object->uploads.push_back(std::move(block));
    return at;
}

namespace state {

void topology(Object* object, std::uint32_t value) {
    with_state(object, [value](host::HostList& host, draw::State& state) { state.set_topology(host, value); });
}

void viewports(Object* object, std::uint32_t count, const D3D12_VIEWPORT* viewports) {
    if (host::HostList* host = host_list(object)) draw::viewports(*host, items(viewports, count));
}

void scissors(Object* object, std::uint32_t count, const RECT* rects) {
    if (host::HostList* host = host_list(object)) draw::scissors(*host, items(rects, count));
}

void blend_factor(Object* object, const float* factor) {
    if (host::HostList* host = host_list(object)) draw::blend_factor(*host, factor);
}

void stencil_ref(Object* object, std::uint32_t reference) {
    if (host::HostList* host = host_list(object)) draw::stencil_ref(*host, reference);
}

void heaps(Object* object, std::uint32_t count, const Object* const* heaps) {
    with_state(object, [count, heaps](host::HostList&, draw::State& value) {
        value.set_heaps(heaps, count);
    });
}

void compute_table(Object* object, std::uint32_t slot, std::uint64_t handle) {
    with_state(object, [slot, handle](host::HostList&, draw::State& value) { value.set_table(true, slot, handle); });
}

void graphics_table(Object* object, std::uint32_t slot, std::uint64_t handle) {
    with_state(object, [slot, handle](host::HostList&, draw::State& value) { value.set_table(false, slot, handle); });
}

void compute_constant(Object* object, std::uint32_t slot, std::uint32_t value_word, std::uint32_t offset) {
    with_state(object, [slot, value_word, offset](host::HostList&, draw::State& value) {
        value.set_constants(true, slot, std::span<const std::uint32_t>(&value_word, 1), offset);
    });
}

void graphics_constant(Object* object, std::uint32_t slot, std::uint32_t value_word, std::uint32_t offset) {
    with_state(object, [slot, value_word, offset](host::HostList&, draw::State& value) {
        value.set_constants(false, slot, std::span<const std::uint32_t>(&value_word, 1), offset);
    });
}

void compute_constants(Object* object, std::uint32_t slot, std::uint32_t count, const std::uint32_t* values,
                       std::uint32_t offset) {
    with_state(object, [slot, count, values, offset](host::HostList&, draw::State& value) {
        value.set_constants(true, slot, items(values, count), offset);
    });
}

void graphics_constants(Object* object, std::uint32_t slot, std::uint32_t count, const std::uint32_t* values,
                        std::uint32_t offset) {
    with_state(object, [slot, count, values, offset](host::HostList&, draw::State& value) {
        value.set_constants(false, slot, items(values, count), offset);
    });
}

void compute_view(Object* object, std::uint32_t slot, std::uint64_t address) {
    with_state(object, [slot, address](host::HostList&, draw::State& value) { value.set_view(true, slot, address); });
}

void graphics_view(Object* object, std::uint32_t slot, std::uint64_t address) {
    with_state(object, [slot, address](host::HostList&, draw::State& value) { value.set_view(false, slot, address); });
}

void index_buffer(Object* object, const draw::IndexView* view) {
    with_state(object, [view](host::HostList&, draw::State& value) { value.set_index_buffer(view); });
}

void vertex_buffers(Object* object, std::uint32_t start, std::uint32_t count, const draw::VertexView* views) {
    with_state(object, [start, count, views](host::HostList&, draw::State& value) {
        if (views == nullptr)
            value.clear_vertex_buffers(start, count);
        else
            value.set_vertex_buffers(start, items(views, count));
    });
}

void targets(Object* object, std::uint32_t count, const std::uint64_t* handles, std::int32_t one_range,
             const std::uint64_t* depth) {
    std::vector<std::uint64_t> all;
    if (one_range != 0 && handles != nullptr) {
        const std::uint64_t first = *handles;
        all.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) all.push_back(first + 128 * index);
    } else {
        const auto listed = items(handles, count);
        all.assign(listed.begin(), listed.end());
    }
    const std::uint64_t depth_value = depth != nullptr ? *depth : 0;
    with_state(object, [&all, depth_value](host::HostList& host, draw::State& value) {
        value.set_targets(host, all, depth_value);
    });
}

void draw(Object* object, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first,
          std::uint32_t first_instance) {
    count_draw();
    with_state(object, [vertices, instances, first, first_instance](host::HostList& host, draw::State& value) {
        value.draw(host, vertices, instances, first, first_instance);
    });
}

void dispatch(Object* object, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    count_dispatch();
    with_state(object, [x, y, z](host::HostList& host, draw::State& value) { value.dispatch(host, x, y, z); });
}

void dispatch_mesh(Object* object, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    count_draw();
    if (y * z != 1) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a mesh dispatch with y or z groups");
        });
    }
    with_state(object, [x](host::HostList& host, draw::State& value) { value.dispatch_mesh(host, x); });
}

void dispatch_mesh_x(Object* object, std::uint32_t x, std::uint64_t, std::uint64_t indirect, std::uint64_t) {
    count_draw();
    if (indirect != 0) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: indirect mesh dispatches dropped");
        });
        return;
    }
    with_state(object, [x](host::HostList& host, draw::State& value) { value.dispatch_mesh(host, x); });
}

void draw_indexed(Object* object, std::uint32_t indices, std::uint32_t instances, std::uint32_t first,
                  std::int32_t base, std::uint32_t first_instance) {
    count_draw();
    with_state(object, [indices, instances, first, base, first_instance](host::HostList& host, draw::State& value) {
        value.draw_indexed(host, indices, instances, first, base, first_instance);
    });
}

}  // namespace state

namespace {

void write32(Object* object, std::uint64_t address, std::uint32_t value, std::uint32_t, std::uint32_t) {
    defer_write(object, address, host::Written::of(value), 4);
}

void write64(Object* object, std::uint64_t address, std::uint64_t value, std::uint32_t, std::uint32_t flags) {
    constexpr std::uint32_t kGpuTimestamp = 1;
    constexpr std::uint32_t kSystemTimestamp = 2;
    host::Written written = host::Written::of(value);
    if (flags == kGpuTimestamp)
        written = host::Written::gpu_timestamp();
    else if (flags == kSystemTimestamp)
        written = host::Written::system_timestamp();
    defer_write(object, address, written, 8);
}

void wait_value(Object*, std::uint64_t, std::uint32_t, std::uint64_t) {}

void barrier_groups(Object* object, std::uint32_t count, const std::uint8_t*) {
    if (count == 0) return;
    if (host::HostList* host = host_list(object)) {
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier();
        host->list()->ResourceBarrier(1, &barrier);
    }
}

void execute_indirect(Object* object, Object* signature, std::uint32_t max, Object* arguments, std::uint64_t offset,
                      Object* count, std::uint64_t count_offset) {
    count_draw();
    const auto layout = signature::layout(signature);
    if (!layout) return;
    std::optional<std::uint64_t> address;
    if (const auto bytes = resource::buffer_bytes(arguments))
        address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes->first)) + offset;
    std::optional<std::uint64_t> counter;
    if (const auto bytes = resource::buffer_bytes(count))
        counter = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes->first)) + count_offset;
    bool ran = false;
    if (layout->arguments != nullptr && layout->arguments->size() == 1 && address &&
        (count == nullptr || counter)) {
        constexpr std::uint32_t kDispatch = 2;
        constexpr std::uint32_t kDispatchSingle = 105;
        const std::uint32_t kind = (*layout->arguments)[0][0];
        const std::uint32_t host_kind = kind == kDispatchSingle ? kDispatch : kind;
        with_state(object, [&ran, layout, host_kind, max, address, counter](host::HostList& host,
                                                                           draw::State& value) {
            ran = value.execute_indirect(host, std::make_tuple(host_kind, layout->stride, max), *address, counter);
        });
    }
    if (ran) return;
    g_indirect.fetch_add(1, std::memory_order_relaxed);
    static std::mutex seen_mutex;
    static std::vector<std::uintptr_t> seen;
    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        const auto key = reinterpret_cast<std::uintptr_t>(signature);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
    }
    G6SHIM_INFO("d3d12_xs: indirect execution dropped (stride %u, %u arguments, max %u)", layout->stride,
                static_cast<unsigned>(layout->arguments != nullptr ? layout->arguments->size() : 0), max);
}

constexpr std::size_t kFirstShiftedSlot = 59;
constexpr std::size_t kNewerSlotShift = 2;

std::size_t newer_slot(std::size_t slot) { return slot >= kFirstShiftedSlot ? slot + kNewerSlotShift : slot; }

std::array<const void*, kMaxSlots> make_list_table(bool newer) {
    std::array<const void*, kMaxSlots> functions = d3d12_object(missing_table<kList, kMaxSlots>());
    const std::array<std::pair<std::size_t, const void*>, 52> methods{{
        {0, reinterpret_cast<const void*>(&list_query)},
        {2, reinterpret_cast<const void*>(&list_release)},
        {7, reinterpret_cast<const void*>(&parent_device)},
        {8, reinterpret_cast<const void*>(&list_type)},
        {9, reinterpret_cast<const void*>(&list_close)},
        {10, reinterpret_cast<const void*>(&list_reset)},
        {12, reinterpret_cast<const void*>(&state::draw)},
        {13, reinterpret_cast<const void*>(&state::draw_indexed)},
        {14, reinterpret_cast<const void*>(&state::dispatch)},
        {15, reinterpret_cast<const void*>(&copy_buffer_region)},
        {16, reinterpret_cast<const void*>(&copy_texture_region)},
        {17, reinterpret_cast<const void*>(&copy_resource)},
        {19, reinterpret_cast<const void*>(&resolve)},
        {20, reinterpret_cast<const void*>(&state::topology)},
        {21, reinterpret_cast<const void*>(&state::viewports)},
        {22, reinterpret_cast<const void*>(&state::scissors)},
        {23, reinterpret_cast<const void*>(&state::blend_factor)},
        {24, reinterpret_cast<const void*>(&state::stencil_ref)},
        {25, reinterpret_cast<const void*>(&set_pipeline)},
        {26, reinterpret_cast<const void*>(&barrier)},
        {32, reinterpret_cast<const void*>(&state::heaps)},
        {33, reinterpret_cast<const void*>(&ignored_state)},
        {34, reinterpret_cast<const void*>(&ignored_state)},
        {35, reinterpret_cast<const void*>(&state::compute_table)},
        {36, reinterpret_cast<const void*>(&state::graphics_table)},
        {37, reinterpret_cast<const void*>(&state::compute_constant)},
        {38, reinterpret_cast<const void*>(&state::graphics_constant)},
        {39, reinterpret_cast<const void*>(&state::compute_constants)},
        {40, reinterpret_cast<const void*>(&state::graphics_constants)},
        {41, reinterpret_cast<const void*>(&state::compute_view)},
        {42, reinterpret_cast<const void*>(&state::graphics_view)},
        {43, reinterpret_cast<const void*>(&state::compute_view)},
        {44, reinterpret_cast<const void*>(&state::graphics_view)},
        {45, reinterpret_cast<const void*>(&state::compute_view)},
        {46, reinterpret_cast<const void*>(&state::graphics_view)},
        {47, reinterpret_cast<const void*>(&state::index_buffer)},
        {48, reinterpret_cast<const void*>(&state::vertex_buffers)},
        {50, reinterpret_cast<const void*>(&state::targets)},
        {51, reinterpret_cast<const void*>(&clear_dsv)},
        {52, reinterpret_cast<const void*>(&clear_rtv)},
        {59, reinterpret_cast<const void*>(&execute_indirect)},
        {60, reinterpret_cast<const void*>(&copy_memory)},
        {68, reinterpret_cast<const void*>(&ignored_state)},
        {69, reinterpret_cast<const void*>(&write32)},
        {70, reinterpret_cast<const void*>(&write64)},
        {71, reinterpret_cast<const void*>(&wait_value)},
        {72, reinterpret_cast<const void*>(&wait_value)},
        {73, reinterpret_cast<const void*>(&ignored_state)},
        {83, reinterpret_cast<const void*>(&copy_texture_region_x)},
        {84, reinterpret_cast<const void*>(&copy_resource_x)},
        {124, reinterpret_cast<const void*>(&ignored_state)},
        {127, reinterpret_cast<const void*>(&state::dispatch_mesh)},
    }};
    const std::array<std::pair<std::size_t, const void*>, 9> more{{
        {28, reinterpret_cast<const void*>(&query_mark)},
        {29, reinterpret_cast<const void*>(&query_mark)},
        {30, reinterpret_cast<const void*>(&resolve_queries)},
        {78, reinterpret_cast<const void*>(&dispatch_x)},
        {98, reinterpret_cast<const void*>(&state::dispatch_mesh_x)},
        {99, reinterpret_cast<const void*>(&driver_hint)},
        {129, reinterpret_cast<const void*>(&sync)},
        {130, reinterpret_cast<const void*>(&sync)},
        {137, reinterpret_cast<const void*>(&reset_x)},
    }};
    for (const auto& [slot, method] : methods) functions[newer ? newer_slot(slot) : slot] = method;
    for (const auto& [slot, method] : more) functions[newer ? newer_slot(slot) : slot] = method;
    if (newer) {
        functions[142] = reinterpret_cast<const void*>(&ignored_state);
        functions[146] = reinterpret_cast<const void*>(&barrier_groups);
        functions[148] = reinterpret_cast<const void*>(&allocate_upload);
    }
    return functions;
}

}  // namespace

const std::array<const void*, kMaxSlots>& functions(bool newer) {
    static const std::array<const void*, kMaxSlots> earlier = make_list_table(false);
    static const std::array<const void*, kMaxSlots> later = make_list_table(true);
    return newer ? later : earlier;
}

std::size_t upload_count(Object* object) {
    if (object == nullptr) return 0;
    List* list_object = title_list(object);
    std::lock_guard<std::mutex> lock(list_object->uploads_mutex);
    return list_object->uploads.size();
}

host::HostList* host_list(Object* object) {
    if (object == nullptr) return nullptr;
    return title_list(object)->host.get();
}

std::pair<std::uint64_t, std::uint64_t> take_counts() {
    return {g_draws.exchange(0, std::memory_order_relaxed), g_dispatches.exchange(0, std::memory_order_relaxed)};
}

HRESULT new_list(std::uint32_t kind, Object* allocator, const Iid* iid, Object** out);

const std::array<const void*, 24>& allocator_functions() {
    static const std::array<const void*, 24> table = [] {
        std::array<const void*, 24> functions = d3d12_object(missing_table<kAllocator, 24>());
        functions[0] = reinterpret_cast<const void*>(&allocator_query);
        functions[2] = reinterpret_cast<const void*>(&allocator_release);
        functions[7] = reinterpret_cast<const void*>(&parent_device);
        functions[8] = reinterpret_cast<const void*>(&allocator_reset);
        functions[9] = reinterpret_cast<const void*>(&ignored_state);
        return functions;
    }();
    return table;
}

HRESULT create_allocator(Object*, std::uint32_t kind, const Iid* iid, Object** out) {
    return hand_out(kAllocator, std::make_unique<Allocator>(allocator_functions().data(), kind), iid,
                    kAllocatorIids, out);
}

HRESULT create_list(Object*, std::uint32_t, std::uint32_t kind, Object* allocator, Object*, const Iid* iid,
                    Object** out) {
    return new_list(kind, allocator, iid, out);
}

HRESULT create_list_x(Object*, const ListDesc* desc, Object* allocator, Object*, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    return new_list(desc->kind, allocator, iid, out);
}

HRESULT new_list(std::uint32_t kind, Object* allocator, const Iid* iid, Object** out) {
    if (allocator == nullptr) return E_INVALIDARG;
    auto list_object = std::make_unique<List>(functions(device::newer()).data(), kind);
    if (host::Host* gpu = host::host()) {
        if (auto made = gpu->new_list())
            list_object->host = std::move(made);
        else {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: no host command list");
            });
        }
    }
    return hand_out(kList, std::move(list_object), iid, kListIids, out);
}

}  // namespace g6shim::d3d12_xs::list
