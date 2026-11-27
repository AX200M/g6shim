// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "descriptor_host.hpp"

#include "frame.hpp"
#include "watch.hpp"
#include "sampler.hpp"
#include "shadow.hpp"
#include "texture.hpp"
#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>
#include <g6shim/shader/emit/emit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace g6shim::d3d12_xs::descriptor_host {

namespace {

constexpr std::uint32_t kBlockSlots = 1 << 16;
constexpr std::uint32_t kBufferDimension = 1;
constexpr std::uint32_t kRawElementBytes = 4;
constexpr std::uint32_t kElementAlignment = 256;
constexpr std::size_t kRecordPage = 64 << 10;
constexpr std::size_t kSyncPages = 8;
constexpr std::uint32_t kCopiedFrames = 8;
constexpr std::uint32_t kTabledRead = 1u << 31;
constexpr DWORD kResetWrites = 1;
constexpr std::uint32_t kImageBaseAddressShift = 8;
constexpr std::uint32_t kBufferResourceLevel = 1u << 24;
constexpr std::uint32_t kBufferIndexBounds = 1u << 28;
constexpr std::uint32_t kBufferByteBounds = 3u << 28;
constexpr std::uint32_t kBufferIdentitySelectors = 4u | (5u << 3) | (6u << 6) | (7u << 9);
constexpr std::uint32_t kBufferUint32Format = 20u << 12;
constexpr std::uint32_t kDepthUnorderedFlags = 0x7;
constexpr std::uint32_t kTextureViews = 2;
constexpr std::uint32_t kTextureViewsEnd = 10;

static_assert(sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == 40);
static_assert(sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC) == 40);

std::mutex g_ranges_mutex;

struct RetiredView {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::uint64_t until = 0;
};

std::mutex g_retire_mutex;
std::vector<RetiredView> g_retired;
std::uint64_t g_retire_clock = 0;

std::uint64_t record_key(std::uint64_t handle) {
    std::array<std::uint32_t, 3> record{};
    std::memcpy(record.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(handle)), sizeof record);
    return static_cast<std::uint64_t>(record[2]) << 32 | record[1];
}

void retire_replaced(Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    if (!resource) return;
    host::retire(Microsoft::WRL::ComPtr<IUnknown>(resource.Get()));
    std::lock_guard<std::mutex> lock(g_retire_mutex);
    g_retired.push_back(RetiredView{std::move(resource), g_retire_clock + 2});
}

struct GpuRange {
    std::uint64_t start;
    std::uint64_t bytes;
    std::uint64_t step;
    bool sampler;
};

std::vector<GpuRange> g_ranges;

void register_range(std::uint64_t start, std::uint64_t bytes, std::uint64_t step, bool sampler) {
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    g_ranges.push_back(GpuRange{start, bytes, step, sampler});
}

using ImageData = g6shim::shader::emit::ImageHeapData;
using BufferData = g6shim::shader::emit::BufferHeapData;

struct HeapData {
    HeapDataBinding binding;
    std::uint8_t* data;
    std::uint32_t count;
};

struct BufferHeapAlias {
    std::uint32_t index;
    std::shared_ptr<shadow::Shadow> backing;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

void refresh_aliases(host::HostList& list, std::span<const BufferHeapAlias> aliases) {
    struct Range {
        std::shared_ptr<shadow::Shadow> backing;
        std::uint64_t from;
        std::uint64_t to;
    };
    std::vector<Range> ranges;
    std::unordered_map<const shadow::Shadow*, std::size_t> known;
    for (const BufferHeapAlias& alias : aliases) {
        if (!alias.backing) continue;
        const std::uint64_t end = alias.address + alias.bytes;
        const auto [found, fresh] = known.try_emplace(alias.backing.get(), ranges.size());
        if (fresh) {
            ranges.push_back(Range{alias.backing, alias.address, end});
            continue;
        }
        Range& range = ranges[found->second];
        range.from = std::min(range.from, alias.address);
        range.to = std::max(range.to, end);
    }
    for (const Range& range : ranges) range.backing->refresh(list, range.from, range.to - range.from);
}

struct BufferHeap {
    HeapData data;
    std::map<std::uint32_t, BufferHeapAlias> aliases;
    std::uint64_t generation = 0;
};

struct SlotBits {
    void reset(std::uint32_t slots) {
        words_ = (static_cast<std::size_t>(slots) + 63) / 64;
        words = std::vector<std::atomic<std::uint64_t>>(words_);
    }

    void mark(std::uint32_t index) {
        const std::size_t word = index / 64;
        if (word >= words_) return;
        words[word].fetch_or(1ull << (index % 64), std::memory_order_release);
    }

    std::uint64_t take(std::uint32_t word) {
        if (word >= words_) return 0;
        if (words[word].load(std::memory_order_acquire) == 0) return 0;
        return words[word].exchange(0, std::memory_order_acq_rel);
    }

    std::uint32_t word_count() const { return static_cast<std::uint32_t>(words_); }

    std::uint64_t peek(std::uint32_t word) const {
        if (word >= words_) return 0;
        return words[word].load(std::memory_order_acquire);
    }

    std::size_t words_ = 0;
    std::vector<std::atomic<std::uint64_t>> words;
};

std::atomic<int> g_lazy{-1};
std::atomic<std::uint64_t> g_marked{0};
std::atomic<std::uint64_t> g_materialized{0};
std::atomic<std::uint64_t> g_skipped{0};

void note_marked() { g_marked.fetch_add(1, std::memory_order_relaxed); }

void note_materialized() { g_materialized.fetch_add(1, std::memory_order_relaxed); }

void note_skipped() { g_skipped.fetch_add(1, std::memory_order_relaxed); }

struct HeapEntry {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    Object* heap = nullptr;
    std::uint32_t count = 0;
    std::uint32_t step = 0;
    bool sampler = false;
    bool tried = false;
    std::unique_ptr<HostHeap> host;
    std::atomic<HostHeap*> ready{nullptr};
    std::optional<HeapData> image_data;
    std::optional<BufferHeap> buffer_data;
    std::uint64_t buffers_refreshed_frame = 0;
    std::vector<std::uint32_t> pending;
    SlotBits dirty;
    SlotBits seen;
    std::atomic<std::uint64_t> seen_eviction{0};
    std::mutex mutex;
    std::mutex copied_mutex;
    std::vector<std::uint8_t> copied;
    std::vector<std::atomic<std::uint32_t>> tabled;
    bool watch_probed = false;
    bool watch_usable = false;
    std::mutex guard_mutex;
    bool guard_usable = true;
    std::uint64_t guard_seen = 0;
};

std::shared_mutex g_heaps_mutex;
std::vector<std::shared_ptr<HeapEntry>> g_heaps;

std::shared_ptr<HeapEntry> entry_for_handle(std::uint64_t handle) {
    std::shared_lock<std::shared_mutex> lock(g_heaps_mutex);
    for (const auto& entry : g_heaps)
        if (handle >= entry->start && handle < entry->end) return entry;
    return nullptr;
}

std::shared_ptr<HeapEntry> entry_for_heap(Object* heap) {
    std::shared_lock<std::shared_mutex> lock(g_heaps_mutex);
    for (const auto& entry : g_heaps)
        if (entry->heap == heap) return entry;
    return nullptr;
}

void create_null_sampler(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    gpu->device()->CreateSampler(&desc, at);
}

D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(const std::array<std::uint32_t, 10>& words) {
    std::array<std::uint32_t, 10> values = words;
    values[0] = resource::pc_format(values[0]);
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    std::memcpy(&desc, values.data(), sizeof desc);
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc(const std::array<std::uint32_t, 10>& words) {
    std::array<std::uint32_t, 10> values = words;
    values[0] = resource::pc_format(values[0]);
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    std::memcpy(&desc, values.data(), sizeof desc);
    return desc;
}

std::uint64_t element_size(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Buffer.StructureByteStride > 0) return desc.Buffer.StructureByteStride;
    if ((static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0) return kRawElementBytes;
    const auto element = resource::element(static_cast<std::uint32_t>(desc.Format));
    return element ? element->first : 0;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> unordered_range(std::uint64_t base,
                                                                       const std::array<std::uint32_t, 10>& words) {
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = uav_desc(words);
    const auto format = resource::element(static_cast<std::uint32_t>(desc.Format));
    const std::uint64_t element =
        desc.Buffer.StructureByteStride > 0
            ? desc.Buffer.StructureByteStride
            : (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0
                  ? kRawElementBytes
                  : format ? format->first : 0;
    if (element == 0) return std::nullopt;
    return std::make_pair(base + desc.Buffer.FirstElement * element,
                          static_cast<std::uint64_t>(desc.Buffer.NumElements) * element);
}

bool empty_buffer_view(std::uint32_t elements) {
    if (elements != 0) return false;
    static std::once_flag once;
    std::call_once(once, [] { G6SHIM_INFO("d3d12_xs: a buffer view with no elements is made a null view"); });
    return true;
}

bool off_elements(std::uint64_t offset, std::uint32_t stride, bool raw, std::uint64_t element) {
    if (raw) return offset % 16 != 0;
    if (stride > 0) return offset % 16 != 0 && offset % stride != 0;
    return element > 0 && offset % element != 0;
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> exact_copy(std::uint64_t address, std::uint64_t bytes,
                                                                 bool unordered) {
    if (bytes == 0 || bytes > (std::uint64_t{1} << 28)) return std::nullopt;
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    auto made = unordered ? gpu->shared_buffer(bytes) : gpu->upload_buffer(bytes);
    if (!made || made->second == nullptr) return std::nullopt;
    std::memcpy(made->second, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
                static_cast<std::size_t>(bytes));
    return std::move(made->first);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> buffer_srv(std::uint64_t address, std::uint64_t bytes,
                                                                 D3D12_SHADER_RESOURCE_VIEW_DESC desc,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (empty_buffer_view(desc.Buffer.NumElements)) return std::nullopt;
    const auto shadow = shadow::find(address, bytes);
    if (!shadow) return std::nullopt;
    (*shadow)->update(static_cast<std::size_t>(address), static_cast<std::size_t>(bytes));
    const std::uint64_t element = element_size(desc);
    const std::uint64_t offset = (*shadow)->offset(address);
    const bool raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    if (element == 0 || off_elements(offset, desc.Buffer.StructureByteStride, raw, element)) {
        auto copy = exact_copy(address, bytes, false);
        if (!copy) return std::nullopt;
        desc.Buffer.FirstElement = 0;
        host::Host* gpu = host::host();
        if (gpu == nullptr) return std::nullopt;
        gpu->device()->CreateShaderResourceView(copy->Get(), &desc, at);
        return copy;
    }
    desc.Buffer.FirstElement += static_cast<UINT>(offset / element);
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    gpu->device()->CreateShaderResourceView((*shadow)->buffer(), &desc, at);
    return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> buffer_uav(std::uint64_t address, std::uint64_t bytes,
                                                                 D3D12_UNORDERED_ACCESS_VIEW_DESC desc,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (empty_buffer_view(desc.Buffer.NumElements)) return std::nullopt;
    const auto shadow = shadow::find(address, bytes);
    if (!shadow) return std::nullopt;
    (*shadow)->update(static_cast<std::size_t>(address), static_cast<std::size_t>(bytes));
    if ((static_cast<std::uint32_t>((*shadow)->buffer()->GetDesc().Flags) &
         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        G6SHIM_WARN("d3d12_xs: unordered access to a buffer whose host copy shaders cannot write");
        return std::nullopt;
    }
    const auto format = resource::element(static_cast<std::uint32_t>(desc.Format));
    const bool raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    const std::uint64_t element =
        desc.Buffer.StructureByteStride > 0
            ? desc.Buffer.StructureByteStride
            : raw ? kRawElementBytes
                  : format ? format->first : 0;
    const std::uint64_t offset = (*shadow)->offset(address);
    if (element == 0 || off_elements(offset, desc.Buffer.StructureByteStride, raw, element)) {
        auto copy = exact_copy(address, bytes, true);
        if (!copy) return std::nullopt;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.CounterOffsetInBytes = 0;
        host::Host* gpu = host::host();
        if (gpu == nullptr) return std::nullopt;
        gpu->device()->CreateUnorderedAccessView(copy->Get(), nullptr, &desc, at);
        return copy;
    }
    desc.Buffer.CounterOffsetInBytes = 0;
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    gpu->device()->CreateUnorderedAccessView((*shadow)->buffer(), nullptr, &desc, at);
    return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
}

bool fits(ID3D12Resource* texture, std::uint32_t* format, bool unordered) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const std::uint32_t flags = static_cast<std::uint32_t>(desc.Flags);
    const bool allowed = unordered ? (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0
                                   : (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    const std::uint32_t own = static_cast<std::uint32_t>(desc.Format);
    const auto depth = std::find_if(resource::kDepthFormats.begin(), resource::kDepthFormats.end(),
                                    [own](const auto& pair) { return pair.first == own; });
    if (!allowed || depth != resource::kDepthFormats.end()) {
        G6SHIM_WARN("d3d12_xs: view the host does not allow (format %u, flags 0x%x)", own, flags);
        return false;
    }
    const bool typeless = resource::family(own) == own && resource::family(own + 1) == own;
    if (format != nullptr && unordered) {
        const auto srgb = std::find_if(resource::kSrgbFormats.begin(), resource::kSrgbFormats.end(),
                                       [format](const auto& pair) { return pair.first == *format; });
        if (srgb != resource::kSrgbFormats.end()) *format = srgb->second;
    }
    const std::uint32_t view = format != nullptr ? *format : 0;
    if (view == 0) return !typeless;
    if (resource::family(view) != resource::family(own)) {
        G6SHIM_WARN("d3d12_xs: view format %u of another family than %u", view, own);
        return false;
    }
    if (format != nullptr && !typeless) *format = own;
    return true;
}

void null_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_buffer_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_raw_uav(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
}

void null_typed_buffer(D3D12_CPU_DESCRIPTOR_HANDLE at, bool unordered) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    if (unordered) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 1;
        gpu->device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = 1;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_structured_buffer(D3D12_CPU_DESCRIPTOR_HANDLE at, bool unordered) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    if (unordered) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 1;
        desc.Buffer.StructureByteStride = 4;
        gpu->device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = 1;
    desc.Buffer.StructureByteStride = 4;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_texture(D3D12_CPU_DESCRIPTOR_HANDLE at, D3D12_SRV_DIMENSION dimension, bool unordered) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    if (unordered) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = dimension == D3D12_SRV_DIMENSION_TEXTURE3D ? D3D12_UAV_DIMENSION_TEXTURE3D
                             : dimension == D3D12_SRV_DIMENSION_TEXTURECUBE
                                 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                 : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) desc.Texture3D.WSize = 1;
        if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) desc.Texture2DArray.ArraySize = 6;
        gpu->device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = dimension;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D) desc.Texture3D.MipLevels = 1;
    else if (dimension == D3D12_SRV_DIMENSION_TEXTURECUBE) desc.TextureCube.MipLevels = 1;
    else desc.Texture2D.MipLevels = 1;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void write_null(g6shim::shader::emit::NullView kind, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    using Kind = g6shim::shader::emit::NullView;
    switch (kind) {
    case Kind::RawSrv: null_buffer_view(at); break;
    case Kind::RawUav: null_raw_uav(at); break;
    case Kind::TypedSrv: null_typed_buffer(at, false); break;
    case Kind::TypedUav: null_typed_buffer(at, true); break;
    case Kind::StructuredSrv: null_structured_buffer(at, false); break;
    case Kind::StructuredUav: null_structured_buffer(at, true); break;
    case Kind::Texture2DSrv: null_texture(at, D3D12_SRV_DIMENSION_TEXTURE2D, false); break;
    case Kind::Texture2DUav: null_texture(at, D3D12_SRV_DIMENSION_TEXTURE2D, true); break;
    case Kind::Texture3DSrv: null_texture(at, D3D12_SRV_DIMENSION_TEXTURE3D, false); break;
    case Kind::Texture3DUav: null_texture(at, D3D12_SRV_DIMENSION_TEXTURE3D, true); break;
    case Kind::TextureCubeSrv: null_texture(at, D3D12_SRV_DIMENSION_TEXTURECUBE, false); break;
    case Kind::TextureCubeUav: null_texture(at, D3D12_SRV_DIMENSION_TEXTURECUBE, true); break;
    case Kind::Count: break;
    }
}

void null_like(const std::array<std::uint32_t, 10>& words, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (words[1] < kTextureViews || words[1] > kTextureViewsEnd) {
        null_buffer_view(at);
        return;
    }
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    std::array<std::uint32_t, 10> values{};
    values[1] = words[1];
    switch (words[1]) {
    case 6: break;
    case 7: values[5] = 1; break;
    case 3:
    case 5:
    case 10: values[5] = 1; values[7] = 1; break;
    default: values[5] = 1; break;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(values);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

bool block_compressed(std::uint32_t format) {
    constexpr std::uint32_t kFirstBlock = DXGI_FORMAT_BC1_TYPELESS;
    constexpr std::uint32_t kLastLegacyBlock = DXGI_FORMAT_BC5_SNORM;
    constexpr std::uint32_t kFirstWideBlock = DXGI_FORMAT_BC6H_TYPELESS;
    constexpr std::uint32_t kLastBlock = DXGI_FORMAT_BC7_UNORM_SRGB;
    return (format >= kFirstBlock && format <= kLastLegacyBlock) || (format >= kFirstWideBlock && format <= kLastBlock);
}

bool views_mips(D3D12_SRV_DIMENSION dimension) {
    return dimension == D3D12_SRV_DIMENSION_TEXTURE2D || dimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY ||
           dimension == D3D12_SRV_DIMENSION_TEXTURECUBE || dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY ||
           dimension == D3D12_SRV_DIMENSION_TEXTURE3D;
}

std::uint32_t usable_base_mip(const resource::ResourceDesc& desc, const D3D12_SHADER_RESOURCE_VIEW_DESC& view) {
    constexpr std::uint32_t kTargetDepthOrUnordered = 0x7;
    constexpr std::uint32_t kBlockTexels = 4;
    if (!views_mips(view.ViewDimension) || (desc.flags & kTargetDepthOrUnordered) != 0 || desc.sample_count > 1 ||
        desc.mip_levels <= 1)
        return 0;
    std::uint32_t base = std::min<std::uint32_t>(view.Texture2D.MostDetailedMip, desc.mip_levels - 1u);
    const bool blocks = block_compressed(resource::pc_format(desc.format));
    for (; base > 0; --base) {
        const std::uint64_t width = desc.width >> base;
        const std::uint32_t height = desc.height >> base;
        if (width == 0 || height == 0) continue;
        if (!blocks || (width % kBlockTexels == 0 && height % kBlockTexels == 0)) break;
    }
    return base;
}

void from_base_mip(D3D12_SHADER_RESOURCE_VIEW_DESC& view, std::uint32_t base_mip) {
    if (base_mip == 0 || !views_mips(view.ViewDimension)) return;
    view.Texture2D.MostDetailedMip -= std::min<UINT>(view.Texture2D.MostDetailedMip, base_mip);
    FLOAT* clamp = nullptr;
    if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) clamp = &view.Texture2D.ResourceMinLODClamp;
    else if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) clamp = &view.Texture2DArray.ResourceMinLODClamp;
    else if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) clamp = &view.TextureCube.ResourceMinLODClamp;
    else if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY)
        clamp = &view.TextureCubeArray.ResourceMinLODClamp;
    else clamp = &view.Texture3D.ResourceMinLODClamp;
    *clamp = std::max(*clamp - static_cast<FLOAT>(base_mip), 0.0f);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> make(const descriptor::View& view,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE at);

void null_for(const descriptor::View& view, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    if (const auto* constant = std::get_if<descriptor::ConstantView>(&view)) {
        (void)constant;
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        gpu->device()->CreateConstantBufferView(&desc, at);
        return;
    }
    using Kind = g6shim::shader::emit::NullView;
    const auto buffer_kind = [](bool unordered, bool raw, bool structured) {
        if (raw) return unordered ? Kind::RawUav : Kind::RawSrv;
        if (structured) return unordered ? Kind::StructuredUav : Kind::StructuredSrv;
        return unordered ? Kind::TypedUav : Kind::TypedSrv;
    };
    const auto texture_kind = [](std::uint32_t resource_dimension, std::uint32_t view_dimension, bool unordered) {
        const bool cube = view_dimension == D3D12_SRV_DIMENSION_TEXTURECUBE ||
                          view_dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        const bool volume = resource_dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
                            view_dimension == D3D12_SRV_DIMENSION_TEXTURE3D ||
                            view_dimension == static_cast<std::uint32_t>(D3D12_UAV_DIMENSION_TEXTURE3D);
        if (cube) return unordered ? Kind::TextureCubeUav : Kind::TextureCubeSrv;
        if (volume) return unordered ? Kind::Texture3DUav : Kind::Texture3DSrv;
        return unordered ? Kind::Texture2DUav : Kind::Texture2DSrv;
    };
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        const bool is_buffer =
            resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource)) != std::nullopt;
        if (is_buffer || (shader->desc && (*shader->desc)[1] == kBufferDimension)) {
            bool raw = true;
            bool structured = false;
            if (shader->desc) {
                const auto desc = srv_desc(*shader->desc);
                raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
                structured = desc.Buffer.StructureByteStride != 0;
            }
            write_null(buffer_kind(false, raw, structured), at);
            return;
        }
        const std::uint32_t view_dimension = shader->desc ? (*shader->desc)[1] : 0;
        write_null(texture_kind(0, view_dimension, false), at);
        return;
    }
    bool unordered = false;
    bool buffer = false;
    bool raw = false;
    bool structured = false;
    std::uint32_t resource_dimension = 0;
    std::uint32_t view_dimension = 0;
    if (const auto* access = std::get_if<descriptor::UnorderedView>(&view)) {
        unordered = true;
        buffer = resource::buffer_bytes(reinterpret_cast<Object*>(access->resource)) != std::nullopt ||
                 (access->desc && (*access->desc)[1] == kBufferDimension);
        if (access->desc) {
            view_dimension = (*access->desc)[1];
            const auto desc = uav_desc(*access->desc);
            raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
            structured = desc.Buffer.StructureByteStride != 0;
        }
    } else if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        resource_dimension = placed->resource[0];
        view_dimension = placed->desc[1];
        if (!placed->unordered && resource_dimension == kBufferDimension) {
            std::array<std::uint32_t, 10> words{};
            std::copy_n(placed->desc.begin(), 10, words.begin());
            const auto desc = srv_desc(words);
            write_null(buffer_kind(false, (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0,
                                   desc.Buffer.StructureByteStride != 0),
                       at);
            return;
        }
        if (!placed->unordered) {
            write_null(texture_kind(resource_dimension, view_dimension, false), at);
            return;
        }
        unordered = true;
        buffer = resource_dimension == kBufferDimension;
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), 10, words.begin());
        const auto desc = uav_desc(words);
        raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
        structured = desc.Buffer.StructureByteStride != 0;
    } else {
        write_null(Kind::RawSrv, at);
        return;
    }
    if (buffer) write_null(buffer_kind(unordered, raw, structured), at);
    else write_null(texture_kind(resource_dimension, view_dimension, unordered), at);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> srv_range(std::uint64_t base,
                                                                 const std::array<std::uint32_t, 10>& words) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(words);
    if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return std::nullopt;
    const std::uint64_t element = element_size(desc);
    if (element == 0 || desc.Buffer.FirstElement > (std::numeric_limits<std::uint64_t>::max() - base) / element)
        return std::nullopt;
    return std::make_pair(base + desc.Buffer.FirstElement * element,
                          static_cast<std::uint64_t>(desc.Buffer.NumElements) * element);
}

std::uint64_t placed_address(const descriptor::PlacedView& view) {
    constexpr std::size_t kAddressLow = 10;
    constexpr std::size_t kAddressHigh = 11;
    return std::uint64_t{view.desc[kAddressLow]} | std::uint64_t{view.desc[kAddressHigh]} << 32;
}

std::optional<descriptor::View> table_view(const descriptor::Heap& heap, std::uint64_t table, std::uint32_t dword,
                                          std::uint32_t kind = descriptor::kViewsKind) {
    if (heap.desc.kind != kind || !heap.index(table))
        return std::nullopt;
    const std::uint32_t step = descriptor::increment_size(nullptr, heap.desc.kind);
    const std::uint64_t offset = sizeof(std::uint32_t) * std::uint64_t{dword};
    if ((table - heap.start) % step != 0 || offset % step != 0 ||
        table > std::numeric_limits<std::uint64_t>::max() - offset || !heap.index(table + offset))
        return std::nullopt;
    return descriptor::view(table + offset);
}

std::optional<std::uint64_t> image_address(const descriptor::View& view) {
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (placed->resource[0] < D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
            placed->resource[0] > D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            return std::nullopt;
        return placed_address(*placed);
    }
    std::uintptr_t address = 0;
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (srv->desc && srv_desc(*srv->desc).ViewDimension <= D3D12_SRV_DIMENSION_BUFFER)
            return std::nullopt;
        address = srv->resource;
    } else if (const auto* uav = std::get_if<descriptor::UnorderedView>(&view)) {
        if (uav->desc && uav_desc(*uav->desc).ViewDimension <= D3D12_UAV_DIMENSION_BUFFER)
            return std::nullopt;
        address = uav->resource;
    } else {
        return std::nullopt;
    }
    if (address == 0) return 0;
    const auto texture = resource::texture_memory(reinterpret_cast<Object*>(address));
    if (!texture || texture->first == nullptr) return std::nullopt;
    return reinterpret_cast<std::uint64_t>(texture->first);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> make(const descriptor::View& view,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    if (const auto* constant = std::get_if<descriptor::ConstantView>(&view)) {
        const auto shadow = shadow::find(constant->address, constant->size);
        if (!shadow) return std::nullopt;
        const std::uint64_t address = (*shadow)->gpu(constant->address);
        if (address % kElementAlignment != 0) {
            G6SHIM_WARN("d3d12_xs: constant buffer not 256-byte aligned on the host");
            return std::nullopt;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = address;
        desc.SizeInBytes = (constant->size + 255) / 256 * 256;
        gpu->device()->CreateConstantBufferView(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
    }
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        Object* object = reinterpret_cast<Object*>(shader->resource);
        ID3D12Resource* texture = texture::host_texture(object);
        if (texture != nullptr) {
            std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> desc;
            if (shader->desc) desc = srv_desc(*shader->desc);
            if (desc) from_base_mip(*desc, resource::host_base_mip(object));
            std::uint32_t format = desc ? static_cast<std::uint32_t>(desc->Format) : 0;
            if (!fits(texture, desc ? &format : nullptr, false)) return std::nullopt;
            if (desc) desc->Format = static_cast<DXGI_FORMAT>(format);
            gpu->device()->CreateShaderResourceView(texture, desc ? &*desc : nullptr, at);
            return Microsoft::WRL::ComPtr<ID3D12Resource>(texture);
        }
        const auto bytes = resource::buffer_bytes(object);
        if (bytes && shader->desc)
            return buffer_srv(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second,
                              srv_desc(*shader->desc), at);
        return std::nullopt;
    }
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view)) {
        Object* object = reinterpret_cast<Object*>(unordered->resource);
        ID3D12Resource* texture = texture::host_texture(object);
        if (texture == nullptr) {
            const auto bytes = resource::buffer_bytes(object);
            if (bytes && unordered->desc)
                return buffer_uav(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second,
                                  uav_desc(*unordered->desc), at);
            return std::nullopt;
        }
        std::optional<D3D12_UNORDERED_ACCESS_VIEW_DESC> desc;
        if (unordered->desc) desc = uav_desc(*unordered->desc);
        if (const auto binding = texture::depth_uav_binding(object, desc ? &*desc : nullptr)) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC made = desc.value_or(D3D12_UNORDERED_ACCESS_VIEW_DESC{});
            if (!desc) made.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            if (binding->twin) {
                const UINT mip = made.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D
                                     ? made.Texture2D.MipSlice
                                     : made.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                           ? made.Texture2DArray.MipSlice
                                           : 0;
                const UINT slice = made.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                       ? made.Texture2DArray.FirstArraySlice
                                       : 0;
                if (mip != 0 || slice != 0) {
                    static std::once_flag once;
                    std::call_once(once, [] {
                        G6SHIM_WARN("d3d12_xs: a depth unordered view past mip 0 slice 0 uses mip 0 slice 0");
                    });
                }
                made = {};
                made.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            } else if (made.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) {
                made.Texture2D.PlaneSlice = binding->plane;
            } else if (made.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) {
                made.Texture2DArray.PlaneSlice = binding->plane;
            }
            made.Format = static_cast<DXGI_FORMAT>(binding->format);
            gpu->device()->CreateUnorderedAccessView(binding->resource, nullptr, &made, at);
            return Microsoft::WRL::ComPtr<ID3D12Resource>(binding->resource);
        }
        std::uint32_t format = desc ? static_cast<std::uint32_t>(desc->Format) : 0;
        if (!fits(texture, desc ? &format : nullptr, true)) return std::nullopt;
        if (desc) desc->Format = static_cast<DXGI_FORMAT>(format);
        gpu->device()->CreateUnorderedAccessView(texture, nullptr, desc ? &*desc : nullptr, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>(texture);
    }
    if (const auto* sampler_view = std::get_if<descriptor::SamplerView>(&view)) {
        D3D12_SAMPLER_DESC desc{};
        std::memcpy(&desc, sampler_view->desc.data(), sizeof desc);
        gpu->device()->CreateSampler(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>();
    }
    if (const auto* console = std::get_if<descriptor::ConsoleSamplerView>(&view)) {
        const D3D12_SAMPLER_DESC desc = sampler::console(console->desc);
        gpu->device()->CreateSampler(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>();
    }
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed == nullptr) return std::nullopt;
    if (const auto texture = placed_texture(view)) return make(*texture, at);
    if (placed->resource[0] != kBufferDimension) {
        G6SHIM_WARN("d3d12_xs: placed unordered texture view not mirrored (dimension %u)", placed->resource[0]);
        return std::nullopt;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(placed->resource[4]) |
                                static_cast<std::uint64_t>(placed->resource[5]) << 32;
    const std::uint64_t address = placed_address(*placed);
    std::array<std::uint32_t, 10> words{};
    std::copy_n(placed->desc.begin(), 10, words.begin());
    if (placed->unordered) return buffer_uav(address, bytes, uav_desc(words), at);
    return buffer_srv(address, bytes, srv_desc(words), at);
}

std::mutex g_placed_mutex;
constexpr std::uint64_t kPlacedIdleFrames = 2;
constexpr std::uint32_t kOwnReference = 1;

struct PlacedTexture {
    Object* object = nullptr;
    std::uint64_t used_frame = 0;
};

using PlacedKey = std::tuple<std::uint64_t, std::array<std::uint32_t, 14>, std::uint32_t>;
std::map<PlacedKey, PlacedTexture> g_placed;
std::uint64_t g_placed_swept_frame = 0;
std::vector<Object*> g_placed_replaced;
std::atomic<std::uint64_t> g_placed_replaced_count{0};
std::atomic<std::uint64_t> g_placed_slots_cleared{0};
std::array<std::atomic<std::uint64_t>, kMipBuckets> g_placed_view_base{};
std::array<std::atomic<std::uint64_t>, kMipBuckets> g_placed_first_resident{};
std::atomic<std::uint64_t> g_placed_unknown_layout{0};

}  // namespace

D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc(std::optional<std::uint32_t> dimension) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (!dimension) return desc;
    switch (*dimension) {
    case 2: desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; break;
    case 3:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray.ArraySize = 1;
        break;
    case 5:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.ArraySize = 1;
        break;
    case 8:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D.WSize = std::numeric_limits<UINT>::max();
        break;
    default: break;
    }
    return desc;
}

HostHeap::HostHeap(Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap, std::uint64_t cpu, std::uint64_t gpu,
                   std::uint32_t step, std::uint32_t count, std::uint32_t dynamic, bool sampler)
    : heap_(std::move(heap)),
      cpu_(cpu),
      gpu_(gpu),
      step_(step),
      count_(count),
      dynamic_(dynamic),
      sampler_(sampler),
      stripes_(std::make_unique<std::mutex[]>(kStripes)),
      made_(count, 0),
      quick_(count),
      strides_(count, 0),
      slot_kind_(count, sampler ? kTextureSlotKind : kBufferSlotKind),
      kept_(static_cast<std::size_t>(count) + dynamic),
      window_generation_(static_cast<std::size_t>(count) + dynamic),
      taken_(dynamic, false) {}

std::mutex& HostHeap::stripe(std::uint32_t index) { return stripes_[index % kStripes]; }

std::uint64_t HostHeap::window_stamp(std::uint32_t index) const {
    if (index >= window_generation_.size()) return kNoWindow;
    return window_generation_[index].load(std::memory_order_relaxed);
}

std::uint8_t HostHeap::kind_at(std::uint32_t index) const {
    return index < slot_kind_.size() ? slot_kind_[index] : 0;
}

void HostHeap::note_kind(std::uint32_t index, bool buffer) {
    if (index < slot_kind_.size()) slot_kind_[index] = buffer ? kBufferSlotKind : kTextureSlotKind;
}

std::uint32_t HostHeap::stride_word(std::uint32_t index) const {
    return index < strides_.size() ? strides_[index] : 0;
}

std::uint64_t HostHeap::rebuilds() const { return rebuilds_.load(std::memory_order_relaxed); }

bool HostHeap::shows_none(std::uint32_t index) const {
    return index >= quick_.size() || quick_[index].load(std::memory_order_acquire) == 0;
}

std::uint64_t HostHeap::shown_key(std::uint32_t index) const {
    return index < quick_.size() ? quick_[index].load(std::memory_order_acquire) : 0;
}


std::unique_ptr<HostHeap> HostHeap::create(bool sampler, std::uint32_t count) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return nullptr;
    const D3D12_DESCRIPTOR_HEAP_TYPE kind =
        sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    const auto make_heap = [gpu, kind](std::uint32_t slots) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = kind;
        desc.NumDescriptors = slots;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        if (FAILED(gpu->device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return heap;
        return heap;
    };
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    std::uint32_t dynamic = 0;
    if (sampler) {
        heap = make_heap(count);
        if (!heap) {
            G6SHIM_WARN("d3d12_xs: no host sampler heap of %u descriptors", count);
            return nullptr;
        }
    } else {
        count = std::min(count, std::numeric_limits<std::uint32_t>::max() - kLargeHeapDynamicSlots);
        if (count >= kDynamicSlots) heap = make_heap(count + kLargeHeapDynamicSlots);
        if (heap) {
            dynamic = kLargeHeapDynamicSlots;
        } else if ((heap = make_heap(count + kDynamicSlots)) != nullptr) {
            dynamic = kDynamicSlots;
        } else if ((heap = make_heap(count + kMinDynamicSlots)) != nullptr) {
            dynamic = kMinDynamicSlots;
        } else if (count + kMinDynamicSlots > kPortableViews) {
            G6SHIM_WARN("d3d12_xs: host view heap capped at %u descriptors", kPortableViews);
            heap = make_heap(kPortableViews);
            count = kPortableViews - kMinDynamicSlots;
            dynamic = kMinDynamicSlots;
        } else {
            G6SHIM_WARN("d3d12_xs: no host view heap of %u descriptors", count + kMinDynamicSlots);
            return nullptr;
        }
        if (!heap) {
            G6SHIM_WARN("d3d12_xs: host view heap capped at %u descriptors", kPortableViews);
            return nullptr;
        }
    }
    const std::uint64_t cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const std::uint64_t address = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const std::uint32_t step = gpu->device()->GetDescriptorHandleIncrementSize(kind);
    register_range(address, static_cast<std::uint64_t>(count + dynamic) * step, step, sampler);
    std::unique_ptr<HostHeap> made(new HostHeap(std::move(heap), cpu, address, step, count, dynamic, sampler));
    if (auto words = gpu->shared_buffer(made->console_words_bytes())) {
        host::name(words->first.Get(), "console descriptor words");
        std::memset(words->second, 0, static_cast<std::size_t>(made->console_words_bytes()));
        made->words_ = std::move(words->first);
        made->words_data_ = words->second;
    }
    if (!made->fill_null(kind, count + dynamic))
        G6SHIM_WARN("d3d12_xs: host descriptor heap not made null");
    made->place_nulls();
    return made;
}

void HostHeap::place_nulls() {
    const auto count = static_cast<std::uint32_t>(g6shim::shader::emit::NullView::Count);
    if (sampler_ || dynamic_ < count) return;
    const std::uint32_t base = dynamic_ - count;
    for (std::uint32_t kind = 0; kind < count; ++kind) {
        taken_[base + kind] = true;
        write_null(static_cast<g6shim::shader::emit::NullView>(kind), cpu(count_ + base + kind));
    }
}

std::uint32_t HostHeap::null_base() const {
    const auto count = static_cast<std::uint32_t>(g6shim::shader::emit::NullView::Count);
    return dynamic_ >= count ? count_ + dynamic_ - count : count_;
}

bool HostHeap::fill_null(D3D12_DESCRIPTOR_HEAP_TYPE kind, std::uint32_t total) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return true;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = kind;
    desc.NumDescriptors = kBlockSlots;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> block;
    if (FAILED(gpu->device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&block)))) return false;
    const D3D12_CPU_DESCRIPTOR_HANDLE start = block->GetCPUDescriptorHandleForHeapStart();
    const auto at = [start, this](std::uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
        handle.ptr += static_cast<SIZE_T>(index) * step_;
        return handle;
    };
    if (sampler_)
        create_null_sampler(start);
    else
        null_buffer_view(start);
    std::uint32_t filled = 1;
    while (filled < kBlockSlots) {
        const std::uint32_t amount = std::min(filled, kBlockSlots - filled);
        gpu->device()->CopyDescriptorsSimple(amount, at(filled), start, kind);
        filled += amount;
    }
    std::uint32_t done = 0;
    while (done < total) {
        const std::uint32_t amount = std::min(kBlockSlots, total - done);
        gpu->device()->CopyDescriptorsSimple(amount, cpu(done), start, kind);
        done += amount;
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE HostHeap::cpu(std::uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = cpu_ + static_cast<SIZE_T>(index) * step_;
    return handle;
}

void HostHeap::null_sampler(std::uint32_t index) { create_null_sampler(cpu(index)); }

void HostHeap::keep(std::uint32_t index, Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    const std::uint64_t stamp =
        resource && shadow::live_loose(resource.Get()) ? shadow::eviction_generation() + 1 : kNoWindow;
    Microsoft::WRL::ComPtr<ID3D12Resource> previous;
    {
        std::lock_guard<std::mutex> lock(kept_mutex_);
        if (index >= kept_.size()) return;
        previous = std::move(kept_[index]);
        kept_[index] = std::move(resource);
        window_generation_[index].store(stamp, std::memory_order_relaxed);
    }
    if (previous && previous.Get() != kept(index).Get()) retire_replaced(std::move(previous));
}

std::uint32_t HostHeap::clear_showing(const std::unordered_set<const ID3D12Resource*>& gone) {
    if (sampler_ || gone.empty()) return 0;
    std::vector<std::uint32_t> showing;
    {
        std::lock_guard<std::mutex> lock(kept_mutex_);
        const std::size_t slots = std::min<std::size_t>(count_, kept_.size());
        for (std::size_t index = 0; index < slots; ++index)
            if (kept_[index] && gone.contains(kept_[index].Get())) showing.push_back(static_cast<std::uint32_t>(index));
    }
    for (const std::uint32_t index : showing) {
        std::lock_guard<std::mutex> lock(stripe(index));
        made_[index] = 0;
        if (index < quick_.size()) quick_[index].store(0, std::memory_order_release);
        make_null(index);
    }
    return static_cast<std::uint32_t>(showing.size());
}

Microsoft::WRL::ComPtr<ID3D12Resource> HostHeap::kept(std::uint32_t index) {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    return index < kept_.size() ? kept_[index] : Microsoft::WRL::ComPtr<ID3D12Resource>();
}

bool HostHeap::window_evicted(std::uint32_t index) {
    const std::uint64_t stamp = window_generation_[index].load(std::memory_order_relaxed);
    if (stamp == kNoWindow) return false;
    const std::uint64_t current = shadow::eviction_generation() + 1;
    if (stamp == current) return false;
    std::lock_guard<std::mutex> lock(kept_mutex_);
    if (shadow::live_loose(kept_[index].Get())) {
        window_generation_[index].store(current, std::memory_order_relaxed);
        return false;
    }
    return true;
}

std::optional<std::uint32_t> HostHeap::dynamic(std::uint32_t count) {
    if (sampler_ || count == 0 || count > dynamic_) return std::nullopt;
    std::lock_guard<std::mutex> lock(next_mutex_);
    const std::size_t needed = count;
    std::optional<std::uint32_t> found;
    std::size_t run = 0;
    std::size_t last = std::numeric_limits<std::size_t>::max();
    for (std::size_t step = 0; step < dynamic_; ++step) {
        const std::size_t at = (static_cast<std::size_t>(next_) + step) % dynamic_;
        if (at != last + 1) run = 0;
        last = at;
        run = taken_[at] ? 0 : run + 1;
        if (run == needed) {
            found = static_cast<std::uint32_t>(at + 1 - needed);
            break;
        }
    }
    if (!found) {
        G6SHIM_WARN("d3d12_xs: no free dynamic descriptor slots for %u", count);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < needed; ++i) taken_[*found + i] = true;
    next_ = (*found + count) % dynamic_;
    return count_ + *found;
}

void HostHeap::release(std::uint32_t first, std::uint32_t count) {
    if (first < count_) return;
    const std::uint32_t at = first - count_;
    std::lock_guard<std::mutex> lock(next_mutex_);
    const auto end = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(at) + count, dynamic_));
    for (std::uint32_t slot = at; slot < end; ++slot) taken_[slot] = false;
}

void HostHeap::make_at(std::uint32_t index, const descriptor::View& view) {
    auto kept = make(view, cpu(index));
    if (!kept) null_for(view, cpu(index));
    keep(index, kept ? *kept : Microsoft::WRL::ComPtr<ID3D12Resource>());
}

void HostHeap::make_null(std::uint32_t index) {
    if (sampler_)
        null_sampler(index);
    else {
        null_buffer_view(cpu(index));
        note_kind(index, true);
    }
    keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
}

void HostHeap::constant_at(std::uint32_t index, std::uint64_t gpu, std::uint32_t bytes) {
    keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
    host::Host* device = host::host();
    if (device == nullptr) return;
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = gpu;
    desc.SizeInBytes = (bytes + 255) / 256 * 256;
    device->device()->CreateConstantBufferView(&desc, cpu(index));
}

void HostHeap::raw_at(std::uint32_t index, Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::uint64_t offset,
                      std::uint64_t bytes) {
    host::Host* device = host::host();
    if (device == nullptr || buffer == nullptr) return;
    if (raw_view_elements(bytes) == 0) {
        null_buffer_view(cpu(index));
        keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = offset / kRawElementBytes;
    desc.Buffer.NumElements = static_cast<UINT>(raw_view_elements(bytes));
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->device()->CreateShaderResourceView(buffer.Get(), &desc, cpu(index));
    keep(index, std::move(buffer));
}

std::uint32_t HostHeap::console_slot_bytes() const {
    return sampler_ ? descriptor_words::kSamplerSlotBytes : descriptor_words::kViewSlotBytes;
}

std::uint64_t HostHeap::console_words_bytes() const {
    return static_cast<std::uint64_t>(std::max<std::uint32_t>(count_, 1)) * console_slot_bytes();
}

void HostHeap::write_console_words(std::uint32_t index, const std::optional<descriptor::View>& view) {
    if (words_data_ == nullptr || index >= count_) return;
    const descriptor_words::Words words = view ? console_words_of(*view) : descriptor_words::Words{};
    std::memcpy(words_data_ + static_cast<std::size_t>(index) * console_slot_bytes(), words.data(),
                std::min<std::size_t>(sizeof words, console_slot_bytes()));
}

std::uint64_t raw_view_elements(std::uint64_t bytes) {
    return std::min<std::uint64_t>(bytes / kRawElementBytes, kMaxRawViewElements);
}

namespace {

descriptor_words::Words buffer_view_words(std::uint64_t base, const std::array<std::uint32_t, 10>& words,
                                          bool unordered) {
    std::uint64_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t stride = 0;
    bool raw = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (unordered) {
        const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = uav_desc(words);
        first = desc.Buffer.FirstElement;
        count = desc.Buffer.NumElements;
        stride = desc.Buffer.StructureByteStride;
        raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
        format = desc.Format;
    } else {
        const D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(words);
        first = desc.Buffer.FirstElement;
        count = desc.Buffer.NumElements;
        stride = desc.Buffer.StructureByteStride;
        raw = (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
        format = desc.Format;
    }
    const auto element = resource::element(static_cast<std::uint32_t>(format));
    const std::uint64_t element_bytes = stride > 0 ? stride : raw ? kRawElementBytes : element ? element->first : 0;
    const std::uint32_t records = raw ? count * kRawElementBytes : count;
    return descriptor_words::buffer(base + first * element_bytes, records, stride);
}

bool cube_view(const std::optional<std::array<std::uint32_t, 10>>& words) {
    if (!words) return false;
    const D3D12_SRV_DIMENSION dimension = srv_desc(*words).ViewDimension;
    return dimension == D3D12_SRV_DIMENSION_TEXTURECUBE || dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
}

descriptor_words::Words image_view_words(const resource::ResourceDesc& desc, std::uint64_t address, bool cube) {
    using descriptor_words::ImageType;
    constexpr std::uint32_t kTexture1d = 2;
    descriptor_words::ImageShape shape;
    shape.width = static_cast<std::uint32_t>(desc.width);
    shape.height = desc.height;
    shape.depth = desc.depth_or_array_size;
    shape.levels = resource::mip_count(desc);
    const bool array = desc.dimension != resource::kTexture3d && desc.depth_or_array_size > 1;
    if (desc.dimension == kTexture1d)
        shape.type = array ? ImageType::k1DArray : ImageType::k1D;
    else if (desc.dimension == resource::kTexture3d)
        shape.type = ImageType::k3D;
    else
        shape.type = array ? ImageType::k2DArray : ImageType::k2D;
    if (cube) shape.type = ImageType::kCube;
    return descriptor_words::image(shape, address);
}

descriptor_words::Words resource_view_words(std::size_t resource,
                                            const std::optional<std::array<std::uint32_t, 10>>& words, bool unordered) {
    Object* object = reinterpret_cast<Object*>(resource);
    if (const auto memory = resource::texture_memory(object))
        return image_view_words(memory->second, reinterpret_cast<std::uint64_t>(memory->first),
                                !unordered && cube_view(words));
    const auto bytes = resource::buffer_bytes(object);
    if (!bytes || !words) return {};
    return buffer_view_words(reinterpret_cast<std::uint64_t>(bytes->first), *words, unordered);
}

}  // namespace

descriptor_words::Words console_words_of(const descriptor::View& view) {
    if (const auto* constant = std::get_if<descriptor::ConstantView>(&view))
        return descriptor_words::buffer(constant->address, constant->size, 0);
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view))
        return resource_view_words(shader->resource, shader->desc, false);
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view))
        return resource_view_words(unordered->resource, unordered->desc, true);
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), words.size(), words.begin());
        const std::uint64_t address = placed_address(*placed);
        if (placed->resource[0] == kBufferDimension) return buffer_view_words(address, words, placed->unordered);
        resource::ResourceDesc desc{};
        std::memcpy(&desc, placed->resource.data(), sizeof desc);
        return image_view_words(desc, address, !placed->unordered && cube_view(words));
    }
    return {};
}

std::uint32_t view_stride(const descriptor::View& view) {
    std::optional<std::array<std::uint32_t, 10>> words;
    bool unordered = false;
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        words = shader->desc;
    } else if (const auto* access = std::get_if<descriptor::UnorderedView>(&view)) {
        words = access->desc;
        unordered = true;
    } else if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (placed->resource[0] != kBufferDimension) return 0;
        words.emplace();
        std::copy_n(placed->desc.begin(), 10, words->begin());
        unordered = placed->unordered;
    }
    if (!words) return 0;
    if (unordered) {
        const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = uav_desc(*words);
        if (desc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER) return 0;
        return desc.Buffer.StructureByteStride;
    }
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(*words);
    if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return 0;
    return desc.Buffer.StructureByteStride;
}


std::optional<descriptor::View> HostHeap::mirror(std::uint32_t index, std::uint64_t handle, bool for_draw) {
    if (index >= count_) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            G6SHIM_WARN("d3d12_xs: title descriptor slot %u past the host heap of %u", index, count_);
        return std::nullopt;
    }
    const std::uint64_t key = record_key(handle);
    if (key != 0 && index < quick_.size() && quick_[index].load(std::memory_order_acquire) == key) {
        const std::uint64_t stamp = window_stamp(index);
        const bool current = stamp == kNoWindow || stamp == shadow::eviction_generation() + 1;
        if (current) {
            if (auto cached = descriptor::view(handle)) return cached;
        }
    }
    std::lock_guard<std::mutex> lock(stripe(index));
    std::optional<descriptor::View> view = descriptor::view(handle);
    const auto publish = [this, index](std::uint64_t shown, std::uint32_t stride) {
        made_[index] = shown;
        if (index < strides_.size()) strides_[index] = stride;
        if (index < quick_.size()) quick_[index].store(shown, std::memory_order_release);
    };
    if (!view) {
        write_console_words(index, std::nullopt);
        if (made_[index] != 0) {
            publish(0, 0);
            make_null(index);
            rebuilds_.fetch_add(1, std::memory_order_relaxed);
            note_materialized();
        } else {
            note_skipped();
        }
        return std::nullopt;
    }
    if (made_[index] == key && window_evicted(index)) made_[index] = 0;
    if (made_[index] != key) {
        write_console_words(index, view);
        const bool later = !for_draw && view_is_buffer(*view);
        const std::uint32_t stride = later ? 0 : view_stride(*view);
        publish(later ? 0 : key, stride);
        rebuilds_.fetch_add(1, std::memory_order_relaxed);
        if (later) {
            null_for(*view, cpu(index));
            note_kind(index, view_shows_buffer(*view));
            keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
        } else {
            std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> kept = make(*view, cpu(index));
            if (!kept && !sampler_) null_for(*view, cpu(index));
            note_kind(index, view_shows_buffer(*view));
            keep(index, kept ? *kept : Microsoft::WRL::ComPtr<ID3D12Resource>());
            note_materialized();
        }
    } else {
        note_skipped();
    }
    return view;
}

bool view_is_buffer(const descriptor::View& view) {
    if (std::holds_alternative<descriptor::ConstantView>(view)) return true;
    if (std::holds_alternative<descriptor::PlacedView>(view)) return true;
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view))
        return resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource)) != std::nullopt;
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view))
        return resource::buffer_bytes(reinterpret_cast<Object*>(unordered->resource)) != std::nullopt;
    return false;
}

bool view_shows_buffer(const descriptor::View& view) {
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view))
        return placed->resource[0] == kBufferDimension;
    return view_is_buffer(view);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_range(const descriptor::View& view) {
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        if (!shader->desc) return std::nullopt;
        const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource));
        if (!bytes) return std::nullopt;
        return srv_range(reinterpret_cast<std::uint64_t>(bytes->first), *shader->desc);
    }
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (placed->resource[0] != kBufferDimension) return std::nullopt;
        const std::uint64_t base = placed_address(*placed);
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), 10, words.begin());
        if (placed->unordered) return unordered_range(base, words);
        return srv_range(base, words);
    }
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view)) {
        if (!unordered->desc) return std::nullopt;
        const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(unordered->resource));
        if (!bytes) return std::nullopt;
        return unordered_range(reinterpret_cast<std::uint64_t>(bytes->first), *unordered->desc);
    }
    return std::nullopt;
}

namespace {

std::optional<std::uint64_t> shader_buffer_address(const descriptor::View& view) {
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (srv->resource == 0) {
            if (!srv->desc || srv_desc(*srv->desc).ViewDimension == D3D12_SRV_DIMENSION_BUFFER) return 0;
            return std::nullopt;
        }
        if (!srv->desc) {
            const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(srv->resource));
            if (!bytes) return std::nullopt;
            return reinterpret_cast<std::uint64_t>(bytes->first);
        }
    } else {
        const auto* placed = std::get_if<descriptor::PlacedView>(&view);
        if (placed == nullptr || placed->unordered) return std::nullopt;
    }
    const auto range = buffer_range(view);
    if (!range) return std::nullopt;
    return range->first;
}

std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> shader_view_desc(const descriptor::View& view) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (!srv->desc) return std::nullopt;
        desc = srv_desc(*srv->desc);
    } else {
        const auto* placed = std::get_if<descriptor::PlacedView>(&view);
        if (placed == nullptr || placed->unordered) return std::nullopt;
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), words.size(), words.begin());
        desc = srv_desc(words);
    }
    return desc;
}

std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> shader_buffer_desc(const descriptor::View& view) {
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed != nullptr && placed->resource[0] != kBufferDimension) return std::nullopt;
    const auto desc = shader_view_desc(view);
    if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return std::nullopt;
    return desc;
}

bool null_shader_buffer(const descriptor::View& view) {
    const auto* srv = std::get_if<descriptor::ShaderView>(&view);
    return srv != nullptr && srv->resource == 0 && (!srv->desc || shader_buffer_desc(view));
}

std::optional<std::uint32_t> structured_buffer_stride(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Format != DXGI_FORMAT_UNKNOWN || desc.Buffer.Flags != D3D12_BUFFER_SRV_FLAG_NONE)
        return std::nullopt;
    const std::uint32_t stride = desc.Buffer.StructureByteStride;
    if (stride < 4 || stride % 4 != 0 || stride > g6shim::shader::emit::UserData::kBufferStrideMask)
        return std::nullopt;
    return stride;
}

bool linear_raw_buffer(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    return desc.Format == DXGI_FORMAT_R32_TYPELESS && desc.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW &&
           desc.Buffer.StructureByteStride == 0;
}

struct TypedBufferFormat {
    DXGI_FORMAT dxgi;
    std::uint32_t gfx10;
    std::uint32_t components;
};

constexpr TypedBufferFormat kTypedBufferFormats[] = {
    {DXGI_FORMAT_R32_UINT, 20, 1}, {DXGI_FORMAT_R32_SINT, 21, 1}, {DXGI_FORMAT_R32_FLOAT, 22, 1},
    {DXGI_FORMAT_R16G16_UNORM, 23, 2}, {DXGI_FORMAT_R16G16_SNORM, 24, 2}, {DXGI_FORMAT_R16G16_UINT, 27, 2},
    {DXGI_FORMAT_R16G16_SINT, 28, 2}, {DXGI_FORMAT_R16G16_FLOAT, 29, 2},
    {DXGI_FORMAT_R8G8B8A8_UNORM, 56, 4}, {DXGI_FORMAT_R8G8B8A8_SNORM, 57, 4},
    {DXGI_FORMAT_R8G8B8A8_UINT, 60, 4}, {DXGI_FORMAT_R8G8B8A8_SINT, 61, 4},
};

const TypedBufferFormat* typed_buffer_format(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Buffer.StructureByteStride != 0 || desc.Buffer.Flags != D3D12_BUFFER_SRV_FLAG_NONE)
        return nullptr;
    const auto found = std::find_if(std::begin(kTypedBufferFormats), std::end(kTypedBufferFormats),
                                    [&](const TypedBufferFormat& format) { return format.dxgi == desc.Format; });
    return found != std::end(kTypedBufferFormats) ? found : nullptr;
}

std::optional<std::uint32_t> buffer_component_mapping(std::uint32_t mapping, std::uint32_t components) {
    constexpr std::uint32_t kMappingMark = D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    if (mapping / kMappingMark != 1) return std::nullopt;
    constexpr std::array<std::uint32_t, 6> kGfx10Selectors{4, 5, 6, 7, 0, 1};
    constexpr std::uint32_t kSelectorBits = 3;
    std::uint32_t word = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto index = static_cast<std::size_t>(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, mapping));
        if (index >= kGfx10Selectors.size()) return std::nullopt;
        const std::uint32_t selector = index >= components && index < 4 ? (index == 3 ? 1u : 0u)
                                                                     : kGfx10Selectors[index];
        word |= selector << (i * kSelectorBits);
    }
    return word;
}

}  // namespace

std::optional<std::uint64_t> table_buffer_address(const descriptor::Heap& heap, std::uint64_t table,
                                                  std::uint32_t dword, bool constant_buffer) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (!constant_buffer) return shader_buffer_address(*view);
    const auto* buffer = std::get_if<descriptor::ConstantView>(&*view);
    if (buffer == nullptr) return std::nullopt;
    return buffer->address;
}

std::optional<std::array<std::uint32_t, 3>> table_constant_buffer_words(const descriptor::Heap& heap,
                                                                       std::uint64_t table, std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto* buffer = std::get_if<descriptor::ConstantView>(&*view);
    if (buffer == nullptr) return std::nullopt;
    if (buffer->address == 0 && buffer->size == 0) return std::array<std::uint32_t, 3>{};
    constexpr std::uint32_t kRecordBytes = 4 * sizeof(std::uint32_t);
    if (buffer->address == 0 || buffer->size == 0 || buffer->size % kRecordBytes != 0 ||
        buffer->size / kRecordBytes > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)
        return std::nullopt;

    using UserData = g6shim::shader::emit::UserData;
    const std::uint32_t high = static_cast<std::uint32_t>(buffer->address >> 32) & UserData::kBufferAddressHighMask;
    return std::array{high | (kRecordBytes << UserData::kBufferStrideShift), buffer->size / kRecordBytes,
                      kBufferIdentitySelectors | kBufferUint32Format | kBufferResourceLevel | kBufferIndexBounds};
}

std::optional<std::uint32_t> table_structured_buffer_high_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto stride = structured_buffer_stride(*desc);
    if (!stride) return std::nullopt;
    using UserData = g6shim::shader::emit::UserData;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    const std::uint32_t high = static_cast<std::uint32_t>(*address >> 32) & UserData::kBufferAddressHighMask;
    return (*stride << UserData::kBufferStrideShift) | high;
}

std::optional<std::uint32_t> table_buffer_record_count(const descriptor::Heap& heap, std::uint64_t table,
                                                       std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    if (linear_raw_buffer(*desc)) {
        if (*address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0 ||
            desc->Buffer.NumElements > UINT32_MAX / kRawElementBytes)
            return std::nullopt;
        return desc->Buffer.NumElements * kRawElementBytes;
    }
    if (structured_buffer_stride(*desc) || typed_buffer_format(*desc) != nullptr)
        return desc->Buffer.NumElements;
    return std::nullopt;
}

std::optional<std::uint32_t> table_linear_raw_buffer_high_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc || !linear_raw_buffer(*desc)) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address || *address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0) return std::nullopt;
    return static_cast<std::uint32_t>(*address >> 32) & g6shim::shader::emit::UserData::kBufferAddressHighMask;
}

std::optional<std::uint32_t> table_untyped_buffer_format_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword, bool structured) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc || desc->Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) return std::nullopt;
    if (structured ? !structured_buffer_stride(*desc) : !linear_raw_buffer(*desc)) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address || (!structured && *address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0)) return std::nullopt;
    return kBufferIdentitySelectors | kBufferUint32Format | kBufferResourceLevel |
           (structured ? kBufferIndexBounds : kBufferByteBounds);
}

std::optional<std::uint32_t> table_typed_buffer_word(const descriptor::Heap& heap, std::uint64_t table,
                                                     std::uint32_t dword, bool high_address) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto* found = typed_buffer_format(*desc);
    if (found == nullptr) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    if (high_address) {
        using UserData = g6shim::shader::emit::UserData;
        constexpr std::uint32_t kTypedElementBytes = 4;
        const std::uint32_t high = static_cast<std::uint32_t>(*address >> 32) & UserData::kBufferAddressHighMask;
        return (kTypedElementBytes << UserData::kBufferStrideShift) | high;
    }
    const auto mapping = buffer_component_mapping(desc->Shader4ComponentMapping, found->components);
    if (!mapping) return std::nullopt;
    constexpr std::uint32_t kDataFormatShift = 12;
    return *mapping | (found->gfx10 << kDataFormatShift) | kBufferResourceLevel | kBufferIndexBounds;
}

std::optional<std::uint32_t> table_buffer_component_mapping(const descriptor::Heap& heap, std::uint64_t table,
                                                            std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    return buffer_component_mapping(desc->Shader4ComponentMapping, 4);
}

std::optional<std::uint32_t> table_image_base_word(const descriptor::Heap& heap, std::uint64_t table,
                                                 std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto address = image_address(*view);
    constexpr std::uint64_t kImageAlignment = 1ull << kImageBaseAddressShift;
    if (!address || *address % kImageAlignment != 0) return std::nullopt;
    return static_cast<std::uint32_t>(*address >> kImageBaseAddressShift);
}

namespace {

std::optional<ImageShapeWords> image_shape(const descriptor::View& view) {
    const auto desc = shader_view_desc(view);
    if (!desc || (desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
                  desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY))
        return std::nullopt;

    resource::ResourceDesc resource_desc{};
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        constexpr std::uint64_t kAlignment = 1ull << kImageBaseAddressShift;
        const std::uint64_t address = placed_address(*placed);
        if (address == 0 || address % kAlignment != 0) return std::nullopt;
        std::memcpy(&resource_desc, placed->resource.data(), sizeof resource_desc);
    } else {
        const auto* srv = std::get_if<descriptor::ShaderView>(&view);
        if (srv == nullptr) return std::nullopt;
        if (srv->resource == 0) return ImageShapeWords{};
        const auto memory = resource::texture_memory(reinterpret_cast<Object*>(srv->resource));
        if (!memory) return std::nullopt;
        resource_desc = memory->second;
    }

    if (resource_desc.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resource_desc.sample_count != 1 ||
        resource_desc.width == 0 || resource_desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        resource_desc.height == 0 || resource_desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return std::nullopt;
    using UserData = g6shim::shader::emit::UserData;
    const std::uint32_t base_mip = desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D
                                       ? desc->Texture2D.MostDetailedMip
                                       : desc->Texture2DArray.MostDetailedMip;
    if (base_mip > (UserData::kImageBaseMipMask >> UserData::kImageBaseMipShift) ||
        (resource_desc.mip_levels != 0 && base_mip >= resource_desc.mip_levels))
        return std::nullopt;
    constexpr std::uint32_t kWidthLowBits = 32u - UserData::kImageWidthLowShift;
    const auto width = static_cast<std::uint32_t>(resource_desc.width - 1);
    const std::uint32_t height = resource_desc.height - 1;
    return ImageShapeWords{width << UserData::kImageWidthLowShift,
                           (width >> kWidthLowBits) | (height << UserData::kImageWidthHighBits),
                           base_mip << UserData::kImageBaseMipShift};
}

void write_image_slot(const HeapData& heap, std::uint32_t index, const std::optional<descriptor::View>& view) {
    if (index >= heap.count) return;
    ImageData::Record record{};
    if (view) {
        if (const auto shape = image_shape(*view)) {
            record[ImageData::kWidthLow] = shape->width_low;
            record[ImageData::kDimensions] = shape->dimensions;
            record[ImageData::kBaseMip] = shape->base_mip;
            record[ImageData::kPresent] = 1;
        }
    }
    std::memcpy(heap.data + static_cast<std::size_t>(index) * ImageData::kBytes, record.data(), ImageData::kBytes);
}

struct IndexedBufferView {
    std::uint32_t stride;
    std::uint32_t records;
    std::uint64_t address;
    std::uint64_t bytes;
};

std::optional<IndexedBufferView> indexed_buffer_view(const descriptor::View& view) {
    const auto desc = shader_buffer_desc(view);
    if (!desc || null_shader_buffer(view)) return std::nullopt;
    const auto range = buffer_range(view);
    if (!range || range->second == 0 || range->second > UINT32_MAX) return std::nullopt;
    std::uint64_t resource_bytes = 0;
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        const auto memory = resource::buffer_bytes(reinterpret_cast<Object*>(srv->resource));
        if (memory) resource_bytes = memory->second;
    } else if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        resource::ResourceDesc description{};
        std::memcpy(&description, placed->resource.data(), sizeof description);
        resource_bytes = description.width;
    }
    const std::uint64_t element = element_size(*desc);
    if (element == 0 || desc->Buffer.FirstElement > resource_bytes / element) return std::nullopt;
    const std::uint64_t offset = desc->Buffer.FirstElement * element;
    if (range->second > resource_bytes - offset) return std::nullopt;
    if (const auto stride = structured_buffer_stride(*desc))
        return IndexedBufferView{*stride, desc->Buffer.NumElements, range->first, range->second};
    if (typed_buffer_format(*desc) != nullptr)
        return IndexedBufferView{kRawElementBytes, desc->Buffer.NumElements, range->first, range->second};
    if (linear_raw_buffer(*desc))
        return IndexedBufferView{0, static_cast<std::uint32_t>(range->second), range->first, range->second};
    return std::nullopt;
}

std::optional<BufferData::Record> buffer_slot(HostHeap& host_heap, BufferHeap& heap, std::uint32_t index,
                                               const IndexedBufferView& view) {
    const auto backing = shadow::find(view.address, view.bytes);
    host::Host* gpu = host::host();
    if (!backing || gpu == nullptr) return std::nullopt;
    const std::uint64_t offset = (*backing)->offset(view.address);
    if (offset % kRawElementBytes != 0) return std::nullopt;
    constexpr std::uint32_t kAlignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
    const auto prefix = static_cast<std::uint32_t>(offset % kAlignment);
    if (view.bytes > UINT32_MAX - prefix) return std::nullopt;
    auto alias = heap.aliases.find(index);
    if (alias == heap.aliases.end()) {
        const auto reserved = host_heap.dynamic(1);
        if (!reserved) return std::nullopt;
        alias = heap.aliases.emplace(index, BufferHeapAlias{*reserved, nullptr}).first;
    }
    if (alias->second.backing != *backing || alias->second.address != view.address ||
        alias->second.bytes != view.bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = (offset - prefix) / kRawElementBytes;
        desc.Buffer.NumElements = static_cast<std::uint32_t>((view.bytes + prefix) / kRawElementBytes);
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        gpu->device()->CreateShaderResourceView((*backing)->buffer(), &desc, host_heap.cpu(alias->second.index));
        alias->second = {alias->second.index, *backing, view.address, view.bytes};
        ++heap.generation;
    }
    return BufferData::Record{alias->second.index + 1, view.stride, prefix, view.records};
}

void write_buffer_slot(HostHeap& host_heap, BufferHeap& heap, std::uint32_t index,
                       const std::optional<descriptor::View>& descriptor) {
    if (index >= heap.data.count) return;
    const auto view = descriptor ? indexed_buffer_view(*descriptor) : std::nullopt;
    const auto record = view ? buffer_slot(host_heap, heap, index, *view) : std::nullopt;
    if (!record) {
        const auto alias = heap.aliases.find(index);
        if (alias != heap.aliases.end() && alias->second.backing) {
            alias->second.backing.reset();
            ++heap.generation;
        }
        if (view || (descriptor && !null_shader_buffer(*descriptor) && shader_buffer_desc(*descriptor))) {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: indexed reads cannot access an unsupported buffer view");
            });
        }
    }
    const BufferData::Record words = record.value_or(BufferData::Record{});
    std::memcpy(heap.data.data + static_cast<std::size_t>(index) * BufferData::kBytes,
                words.data(), BufferData::kBytes);
}

std::optional<HeapData> create_heap_data(HeapEntry& entry) {
    host::Host* gpu = host::host();
    if (!entry.host || gpu == nullptr) return std::nullopt;
    const std::uint32_t count = std::min(entry.count, entry.host->count());
    if (count == 0) return std::nullopt;
    const auto reserved = entry.host->dynamic(1);
    if (!reserved) return std::nullopt;
    static_assert(ImageData::kBytes == BufferData::kBytes);
    auto upload = gpu->upload_buffer(static_cast<std::uint64_t>(count) * ImageData::kBytes);
    if (!upload) {
        entry.host->release(*reserved, 1);
        return std::nullopt;
    }
    HeapData data{{*reserved, std::move(upload->first)}, upload->second, count};
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = count * ImageData::kWords;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gpu->device()->CreateShaderResourceView(data.binding.resource.Get(), &desc, entry.host->cpu(*reserved));
    return data;
}

}  // namespace

std::optional<ImageShapeWords> table_image_shape(const descriptor::Heap& heap, std::uint64_t table,
                                                 std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    return view ? image_shape(*view) : std::nullopt;
}

std::optional<std::uint32_t> table_image_sampling_fields(const descriptor::Heap& heap, std::uint64_t table,
                                                        std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto desc = shader_view_desc(*view);
    if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return std::nullopt;
    const auto address = image_address(*view);
    if (!address || *address % (1ull << kImageBaseAddressShift) != 0) return std::nullopt;
    constexpr std::uint32_t kSamplingPerformance = 4;
    return *address == 0 ? 0u : kSamplingPerformance << g6shim::shader::emit::UserData::kImageSamplingPerformanceShift;
}

std::optional<std::uint32_t> sampler_lod_word(const descriptor::Heap& heap, std::uint32_t dword) {
    const auto view = table_view(heap, heap.start, dword, descriptor::kSamplersKind);
    if (!view) return std::nullopt;
    const auto* sampler = std::get_if<descriptor::SamplerView>(&*view);
    if (sampler == nullptr) return std::nullopt;
    D3D12_SAMPLER_DESC desc{};
    std::memcpy(&desc, sampler->desc.data(), sizeof desc);
    if (!std::isfinite(desc.MinLOD) || !std::isfinite(desc.MaxLOD) || desc.MinLOD > desc.MaxLOD)
        return std::nullopt;
    constexpr std::uint32_t kLodFieldBits = 12;
    constexpr float kLodScale = 256.0f;
    constexpr float kMaximumLod = static_cast<float>((1u << kLodFieldBits) - 1u) / kLodScale;
    const auto encode = [=](float lod) {
        return static_cast<std::uint32_t>(std::clamp(lod, 0.0f, kMaximumLod) * kLodScale);
    };
    return encode(desc.MinLOD) | (encode(desc.MaxLOD) << kLodFieldBits);
}

void note_placed_mips(const descriptor::PlacedView& placed, std::uint64_t address,
                      const resource::ResourceDesc& desc);

std::optional<descriptor::View> placed_texture(const descriptor::View& view) {
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed == nullptr || placed->unordered || placed->resource[0] == kBufferDimension) return std::nullopt;
    const std::uint64_t address = placed_address(*placed);
    resource::ResourceDesc desc{};
    std::memcpy(&desc, placed->resource.data(), sizeof desc);
    std::array<std::uint32_t, 10> view_words{};
    std::copy_n(placed->desc.begin(), view_words.size(), view_words.begin());
    const std::uint32_t base_mip = usable_base_mip(desc, srv_desc(view_words));
    const PlacedKey key{address, placed->resource, base_mip};
    {
        std::lock_guard<std::mutex> lock(g_placed_mutex);
        const auto found = g_placed.find(key);
        if (found != g_placed.end()) {
            found->second.used_frame = frame::current_frame();
            Object* object = found->second.object;
            if (!resource::host_resource(object)) return std::nullopt;
            std::array<std::uint32_t, 10> words{};
            std::copy_n(placed->desc.begin(), 10, words.begin());
            return descriptor::View{descriptor::ShaderView{reinterpret_cast<std::size_t>(object), words}};
        }
    }
    Object* out = nullptr;
    if (FAILED(resource::create_at_address_from_mip(address, &desc, base_mip, &out)) || out == nullptr)
        return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(g_placed_mutex);
        const auto [at, fresh] = g_placed.try_emplace(key, PlacedTexture{out, frame::current_frame()});
        if (fresh) note_placed_mips(*placed, address, desc);
        if (!fresh) {
            resource::release_resource(out);
            out = at->second.object;
            at->second.used_frame = frame::current_frame();
        }
    }
    if (!resource::host_resource(out)) return std::nullopt;
    std::array<std::uint32_t, 10> words{};
    std::copy_n(placed->desc.begin(), 10, words.begin());
    return descriptor::View{descriptor::ShaderView{reinterpret_cast<std::size_t>(out), words}};
}

std::size_t placed_texture_count() {
    std::lock_guard<std::mutex> lock(g_placed_mutex);
    return g_placed.size();
}

PlacedStats placed_stats() {
    PlacedStats stats;
    stats.replaced = g_placed_replaced_count.load(std::memory_order_relaxed);
    stats.slots_cleared = g_placed_slots_cleared.load(std::memory_order_relaxed);
    for (std::size_t bucket = 0; bucket < kMipBuckets; ++bucket) {
        stats.view_base_mip[bucket] = g_placed_view_base[bucket].load(std::memory_order_relaxed);
        stats.first_resident_mip[bucket] = g_placed_first_resident[bucket].load(std::memory_order_relaxed);
    }
    stats.unknown_layout = g_placed_unknown_layout.load(std::memory_order_relaxed);
    return stats;
}

void note_placed_mips(const descriptor::PlacedView& placed, std::uint64_t address,
                      const resource::ResourceDesc& desc) {
    std::array<std::uint32_t, 10> words{};
    std::copy_n(placed.desc.begin(), words.size(), words.begin());
    const D3D12_SHADER_RESOURCE_VIEW_DESC view = srv_desc(words);
    const std::size_t base = std::min<std::size_t>(view.Texture2D.MostDetailedMip, kMipBuckets - 1);
    g_placed_view_base[base].fetch_add(1, std::memory_order_relaxed);
    const auto layout = resource::texture_layout(desc);
    if (!layout || layout->planes.empty() || layout->planes[0].layout.mips.empty()) {
        g_placed_unknown_layout.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto& mips = layout->planes[0].layout.mips;
    std::size_t resident = mips.size();
    for (std::size_t mip = 0; mip < mips.size(); ++mip) {
        const std::uint64_t bytes =
            std::max<std::uint64_t>(std::uint64_t{mips[mip].pitch} * mips[mip].rows * std::max(mips[mip].depth, 1u), 1);
        if (texture::committed(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address + mips[mip].offset)),
                               static_cast<std::size_t>(bytes))) {
            resident = mip;
            break;
        }
    }
    g_placed_first_resident[std::min(resident, kMipBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
}

std::size_t expire_placed_textures() {
    const std::uint64_t now = frame::current_frame();
    std::vector<Object*> idle;
    std::vector<Object*> replaced;
    {
        std::lock_guard<std::mutex> lock(g_placed_mutex);
        if (g_placed_swept_frame == now) return 0;
        g_placed_swept_frame = now;
        replaced.swap(g_placed_replaced);
        for (auto it = g_placed.begin(); it != g_placed.end();) {
            ID3D12Resource* shown = texture::host_texture(it->second.object);
            if (now - it->second.used_frame < kPlacedIdleFrames ||
                (shown != nullptr && host::references(shown) > kOwnReference)) {
                ++it;
                continue;
            }
            idle.push_back(it->second.object);
            it = g_placed.erase(it);
        }
    }
    if (!replaced.empty()) {
        std::unordered_set<const ID3D12Resource*> gone;
        for (Object* object : replaced)
            if (ID3D12Resource* shown = texture::host_texture(object)) gone.insert(shown);
        std::vector<std::shared_ptr<HeapEntry>> heaps;
        {
            std::shared_lock<std::shared_mutex> lock(g_heaps_mutex);
            heaps = g_heaps;
        }
        std::uint64_t cleared = 0;
        for (const std::shared_ptr<HeapEntry>& entry : heaps)
            if (entry && entry->host) cleared += entry->host->clear_showing(gone);
        g_placed_replaced_count.fetch_add(replaced.size(), std::memory_order_relaxed);
        g_placed_slots_cleared.fetch_add(cleared, std::memory_order_relaxed);
        for (Object* object : replaced) resource::release_resource(object);
    }
    for (Object* object : idle) resource::release_resource(object);
    return idle.size() + replaced.size();
}

void note_shader_heap(std::uint64_t start, std::uint64_t bytes, std::uint64_t step, bool sampler) {
    if (start == 0 || bytes == 0) return;
    register_range(start, bytes, step == 0 ? 1 : step, sampler);
}

void collect_shader_heaps(std::vector<ShaderHeapRange>& out) {
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    out.reserve(out.size() + g_ranges.size());
    for (const GpuRange& range : g_ranges)
        out.push_back(ShaderHeapRange{range.start, range.bytes, range.sampler});
}

std::string near_heaps(std::uint64_t va) {
    std::string text;
    char line[192] = {};
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    for (const GpuRange& range : g_ranges) {
        const char* kind = range.sampler ? "sampler" : "view";
        if (va < range.start) {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: %llx below", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>(range.start - va));
        } else if (va < range.start + range.bytes) {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: inside, slot %llu", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>((va - range.start) / range.step));
        } else {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: %llx past its end (slot %llu)", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>(va - range.start - range.bytes),
                          static_cast<unsigned long long>((va - range.start) / range.step));
        }
        text += line;
    }
    return text;
}

std::mutex g_early_mutex;
std::vector<std::uint64_t> g_early;

void note_early(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_early_mutex);
    if (g_early.size() < (1u << 20)) g_early.push_back(handle);
}

void register_heap(Object* heap, std::uint64_t start, std::uint64_t end) {
    if (heap == nullptr || end <= start) return;
    const auto* descriptor_heap = static_cast<const descriptor::Heap*>(heap);
    const auto step = descriptor::increment(descriptor_heap->desc.kind);
    if (!step) return;
    auto entry = std::make_shared<HeapEntry>();
    entry->start = start;
    entry->end = end;
    entry->heap = heap;
    entry->count = descriptor_heap->desc.count;
    entry->step = *step;
    entry->sampler = descriptor_heap->desc.kind == descriptor::kSamplersKind;
    entry->dirty.reset(entry->count);
    entry->seen.reset(entry->count);
    entry->tabled = std::vector<std::atomic<std::uint32_t>>(entry->count);
    if (!entry->sampler) {
        for (std::uint32_t index = 0; index < entry->count; ++index) {
            const std::uint64_t handle = start + static_cast<std::uint64_t>(index) * entry->step;
            if (!descriptor::view(handle)) continue;
            entry->dirty.mark(index);
            entry->seen.mark(index);
        }
    }
    {
        std::lock_guard<std::mutex> early(g_early_mutex);
        std::vector<std::uint64_t> left;
        left.reserve(g_early.size());
        for (const std::uint64_t handle : g_early) {
            if (handle < start || handle >= end || entry->step == 0) {
                left.push_back(handle);
                continue;
            }
            const std::uint32_t index = static_cast<std::uint32_t>((handle - start) / entry->step);
            if (index >= entry->count) continue;
            entry->dirty.mark(index);
            entry->seen.mark(index);
        }
        g_early.swap(left);
    }
    std::unique_lock<std::shared_mutex> lock(g_heaps_mutex);
    g_heaps.push_back(std::move(entry));
}

void drop_heap(Object* heap) {
    std::unique_lock<std::shared_mutex> lock(g_heaps_mutex);
    g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(),
                                 [heap](const auto& entry) { return entry->heap == heap; }),
                  g_heaps.end());
}

bool lazy_descriptors();

void mirror(std::uint64_t handle) {
    std::shared_ptr<HeapEntry> entry = entry_for_handle(handle);
    if (!entry || entry->step == 0) {
        note_early(handle);
        return;
    }
    const std::uint32_t index = static_cast<std::uint32_t>((handle - entry->start) / entry->step);
    if (index >= entry->count) return;
    entry->seen.mark(index);
    if (lazy_descriptors()) {
        entry->dirty.mark(index);
        note_marked();
        return;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->host) {
        if (!entry->tried) entry->pending.push_back(index);
        return;
    }
    const auto view = entry->host->mirror(index, handle, true);
    if (entry->image_data) write_image_slot(*entry->image_data, index, view);
    if (entry->buffer_data) write_buffer_slot(*entry->host, *entry->buffer_data, index, view);
}

void release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count) {
    std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry) return;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->host) entry->host->release(first, count);
}

HostHeap* for_heap(Object* heap) {
    std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry) return nullptr;
    if (HostHeap* ready = entry->ready.load(std::memory_order_acquire)) return ready;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->host && !entry->tried) {
        entry->tried = true;
        entry->host = HostHeap::create(entry->sampler, entry->count);
    }
    if (!entry->host) return nullptr;
    std::vector<std::uint32_t> pending = std::move(entry->pending);
    entry->pending.clear();
    for (const std::uint32_t index : pending)
        entry->host->mirror(index, entry->start + static_cast<std::uint64_t>(index) * entry->step, true);
    entry->ready.store(entry->host.get(), std::memory_order_release);
    return entry->host.get();
}

Object* largest_heap(bool samplers) {
    std::shared_lock<std::shared_mutex> lock(g_heaps_mutex);
    Object* best = nullptr;
    std::uint32_t count = 0;
    for (const auto& entry : g_heaps) {
        if (!entry || entry->sampler != samplers || entry->host == nullptr || entry->count <= count) continue;
        best = entry->heap;
        count = entry->count;
    }
    return best;
}

std::optional<HeapDataBinding> image_heap_data(Object* heap) {
    const auto entry = entry_for_heap(heap);
    if (!entry || entry->sampler) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->image_data) return entry->image_data->binding;
    auto data = create_heap_data(*entry);
    if (!data) return std::nullopt;
    for (std::uint32_t index = 0; index < data->count; ++index) {
        const std::uint64_t handle = entry->start + static_cast<std::uint64_t>(index) * entry->step;
        write_image_slot(*data, index, descriptor::view(handle));
    }
    entry->image_data = std::move(data);
    return entry->image_data->binding;
}

std::optional<HeapDataBinding> buffer_heap_data(Object* heap, host::HostList& list,
                                                std::span<const std::uint32_t> named) {
    const auto entry = entry_for_heap(heap);
    if (!entry || entry->sampler) return std::nullopt;
    std::vector<BufferHeapAlias> stale;
    HeapDataBinding binding;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->buffer_data) {
            auto data = create_heap_data(*entry);
            if (!data) return std::nullopt;
            auto& buffers = entry->buffer_data.emplace(BufferHeap{std::move(*data), {}});
            for (std::uint32_t index = 0; index < buffers.data.count; ++index) {
                const std::uint64_t handle = entry->start + static_cast<std::uint64_t>(index) * entry->step;
                write_buffer_slot(*entry->host, buffers, index, descriptor::view(handle));
            }
        }
        const std::uint64_t frame_stamp = frame::current_frame() + 1;
        const auto& aliases = entry->buffer_data->aliases;
        if (entry->buffers_refreshed_frame != frame_stamp) {
            entry->buffers_refreshed_frame = frame_stamp;
            for (const auto& item : aliases)
                if (item.second.backing) stale.push_back(item.second);
        } else {
            for (const std::uint32_t slot : named) {
                const auto found = aliases.find(slot);
                if (found != aliases.end() && found->second.backing) stale.push_back(found->second);
            }
        }
        binding = entry->buffer_data->data.binding;
    }
    refresh_aliases(list, stale);
    return binding;
}

namespace {

class Workers {
public:
    static Workers& shared() {
        static Workers* const made = new Workers;
        return *made;
    }

    std::size_t width() const { return kWidth; }

    void run(std::size_t jobs, const std::function<void(std::size_t)>& job) {
        if (jobs == 0) return;
        std::lock_guard<std::mutex> turn(turn_);
        start();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            jobs_ = jobs;
            next_ = 0;
            left_ = jobs;
            ++round_;
        }
        wake_.notify_all();
        work();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return left_ == 0; });
        job_ = nullptr;
    }

private:
    static constexpr std::size_t kWidth = 8;

    void start() {
        if (started_) return;
        started_ = true;
        unsigned threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 2;
        threads = std::min<unsigned>(threads, kWidth);
        for (unsigned index = 1; index < threads; ++index) std::thread([this] { loop(); }).detach();
    }

    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return round_ != seen; });
                seen = round_;
            }
            work();
        }
    }

    void work() {
        for (;;) {
            const std::function<void(std::size_t)>* job = nullptr;
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job_ == nullptr || next_ >= jobs_) return;
                job = job_;
                index = next_++;
            }
            (*job)(index);
            std::lock_guard<std::mutex> lock(mutex_);
            if (--left_ == 0) done_.notify_all();
        }
    }

    std::mutex turn_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(std::size_t)>* job_ = nullptr;
    std::size_t jobs_ = 0;
    std::size_t next_ = 0;
    std::size_t left_ = 0;
    std::uint64_t round_ = 0;
    bool started_ = false;
};

void materialize_indices(HostHeap& host, std::uint64_t start, std::uint32_t step,
                         std::span<const std::uint32_t> slots) {
    const auto one = [&](std::uint32_t index) {
        host.mirror(index, start + static_cast<std::uint64_t>(index) * step, true);
    };
    constexpr std::size_t kParallelAt = 1024;
    if (slots.size() < kParallelAt) {
        for (const std::uint32_t index : slots) one(index);
        return;
    }
    Workers& workers = Workers::shared();
    const std::size_t chunk = (slots.size() + workers.width() - 1) / workers.width();
    const std::size_t chunks = (slots.size() + chunk - 1) / chunk;
    workers.run(chunks, [&](std::size_t part) {
        const std::size_t from = part * chunk;
        const std::size_t to = std::min(slots.size(), from + chunk);
        for (std::size_t i = from; i < to; ++i) one(slots[i]);
    });
}

void note_evicted(HeapEntry& entry) {
    HostHeap* host = entry.host.get();
    if (host == nullptr) return;
    const std::uint64_t generation = shadow::eviction_generation();
    const std::uint64_t seen = entry.seen_eviction.exchange(generation, std::memory_order_acq_rel);
    if (seen == generation) return;
    const std::uint64_t current = generation + 1;
    const std::uint32_t count = host->count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t stamp = host->window_stamp(index);
        if (stamp != 0 && stamp != current) entry.dirty.mark(index);
    }
}

void collect_dirty(HeapEntry& entry, std::vector<std::uint32_t>& slots) {
    const std::uint32_t words = entry.dirty.word_count();
    for (std::uint32_t word = 0; word < words; ++word) {
        std::uint64_t bits = entry.dirty.take(word);
        while (bits != 0) {
            const auto bit = static_cast<unsigned>(std::countr_zero(bits));
            const std::uint32_t index = word * 64 + bit;
            if (index < entry.count) slots.push_back(index);
            bits &= bits - 1;
        }
    }
}

void apply_heap_data(HeapEntry& entry, std::span<const std::uint32_t> slots,
                     std::vector<BufferHeapAlias>& refresh) {
    if (!entry.image_data && !entry.buffer_data) return;
    std::lock_guard<std::mutex> lock(entry.mutex);
    if (entry.host == nullptr) return;
    for (const std::uint32_t index : slots) {
        const std::uint64_t handle = entry.start + static_cast<std::uint64_t>(index) * entry.step;
        const auto view = descriptor::view(handle);
        if (entry.image_data) write_image_slot(*entry.image_data, index, view);
        if (entry.buffer_data) {
            write_buffer_slot(*entry.host, *entry.buffer_data, index, view);
            const auto alias = entry.buffer_data->aliases.find(index);
            if (alias != entry.buffer_data->aliases.end() && alias->second.backing) refresh.push_back(alias->second);
        }
    }
}

bool shows_other_kind(const HeapEntry& entry, std::uint32_t index, const descriptor::View& view) {
    if (entry.sampler || entry.host == nullptr) return false;
    if (std::holds_alternative<descriptor::SamplerView>(view) ||
        std::holds_alternative<descriptor::ConsoleSamplerView>(view))
        return false;
    const std::uint8_t host = entry.host->kind_at(index);
    return host != 0 && host != (view_shows_buffer(view) ? kBufferSlotKind : kTextureSlotKind);
}

bool tabled_recently(const HeapEntry& entry, std::uint32_t index) {
    if (index >= entry.tabled.size()) return false;
    const std::uint32_t stamp = entry.tabled[index].load(std::memory_order_relaxed) & ~kTabledRead;
    const std::uint32_t now = (static_cast<std::uint32_t>(frame::current_frame()) + 1) & ~kTabledRead;
    return stamp != 0 && now - stamp <= kCopiedFrames;
}

void log_kind_mismatches(HeapEntry& entry) {
    if (!settings::enabled("G6SHIM_D3D_DRED") || entry.host == nullptr || entry.sampler) return;
    std::uint32_t mismatches = 0;
    std::uint32_t tabled = 0;
    std::uint32_t unchanged = 0;
    const std::uint32_t words = entry.seen.word_count();
    for (std::uint32_t word = 0; word < words; ++word) {
        std::uint64_t bits = entry.seen.peek(word);
        while (bits != 0) {
            const auto bit = static_cast<unsigned>(std::countr_zero(bits));
            const std::uint32_t index = word * 64 + bit;
            bits &= bits - 1;
            if (index >= entry.count) continue;
            const std::uint64_t handle = entry.start + static_cast<std::uint64_t>(index) * entry.step;
            const auto view = descriptor::view(handle);
            if (!view || !shows_other_kind(entry, index, *view)) continue;
            ++mismatches;
            if (tabled_recently(entry, index)) ++tabled;
            if (entry.host->shown_key(index) == record_key(handle)) ++unchanged;
        }
    }
    if (mismatches != 0)
        G6SHIM_WARN("d3d12_xs: indexed heap %p has %u slots whose host kind differs from the title (%u read by "
                    "tables lately, %u with the record they were made from)",
                    static_cast<void*>(entry.heap), mismatches, tabled, unchanged);
}

bool probe_watch(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return false;
    void* page = nullptr;
    ULONG_PTR count = 1;
    DWORD granularity = 0;
    if (::GetWriteWatch(0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(start)), static_cast<SIZE_T>(bytes),
                        &page, &count, &granularity) == 0)
        return true;
    return ::GetLastError() == ERROR_INSUFFICIENT_BUFFER;
}

std::vector<std::pair<std::size_t, std::size_t>> take_watch(std::uint64_t start, std::uint64_t end) {
    const auto bytes = static_cast<SIZE_T>(end - start);
    std::size_t capacity = static_cast<std::size_t>(bytes / 4096) + 1;
    std::vector<void*> pages(capacity);
    ULONG_PTR count = capacity;
    DWORD granularity = 0;
    auto ask = [&] {
        count = pages.size();
        return ::GetWriteWatch(kResetWrites, reinterpret_cast<void*>(static_cast<std::uintptr_t>(start)), bytes,
                               pages.data(), &count, &granularity) == 0;
    };
    if (!ask()) {
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || count == 0) return {};
        pages.resize(count);
        if (!ask()) return {};
    }
    if (granularity == 0) granularity = 4096;
    std::vector<std::size_t> written(count);
    for (ULONG_PTR i = 0; i < count; ++i) written[i] = reinterpret_cast<std::size_t>(pages[i]);
    std::sort(written.begin(), written.end());
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const auto heap_start = static_cast<std::size_t>(start);
    const auto heap_end = static_cast<std::size_t>(end);
    for (const std::size_t page : written) {
        const std::size_t from = std::max(page, heap_start);
        const std::size_t to = std::min(page + static_cast<std::size_t>(granularity), heap_end);
        if (from >= to) continue;
        if (!ranges.empty() && ranges.back().second == from)
            ranges.back().second = to;
        else
            ranges.emplace_back(from, to);
    }
    return ranges;
}

void changed_records(std::uint8_t* last, const std::uint8_t* now, std::size_t bytes, std::size_t step,
                     std::uint32_t first, std::vector<std::uint32_t>& out) {
    constexpr std::size_t kAtATime = 16;
    const std::size_t block = kAtATime * step;
    for (std::size_t offset = 0; offset + step <= bytes;) {
        const std::size_t whole = std::min(block, bytes - offset);
        const std::size_t aligned = whole - whole % step;
        if (std::memcmp(last + offset, now + offset, aligned) != 0) {
            for (std::size_t n = 0; n + step <= aligned; n += step) {
                if (std::memcmp(last + offset + n, now + offset + n, step) != 0) {
                    std::memcpy(last + offset + n, now + offset + n, step);
                    out.push_back(first + static_cast<std::uint32_t>((offset + n) / step));
                }
            }
        }
        offset += aligned;
    }
}

void compare_part(HeapEntry& entry, std::size_t from, std::size_t to, std::vector<std::uint32_t>& out) {
    const auto start = static_cast<std::size_t>(entry.start);
    const std::uint64_t step = entry.step;
    if (from < start || to <= from || step == 0) return;
    const std::uint64_t byte_from = static_cast<std::uint64_t>(from - start);
    const std::uint64_t byte_to = static_cast<std::uint64_t>(to - start);
    const std::uint64_t first = (byte_from + step - 1) / step;
    const std::uint64_t last = std::min((byte_to + step - 1) / step, static_cast<std::uint64_t>(entry.count));
    if (first >= last) return;
    const std::size_t record_from = start + static_cast<std::size_t>(first * step);
    if (record_from < from || record_from >= to) return;
    const std::size_t available = to - record_from;
    const std::size_t count_bytes = std::min(static_cast<std::size_t>((last - first) * step), available - available % step);
    if (count_bytes < step) return;
    thread_local std::vector<std::uint8_t> page;
    page.resize(count_bytes);
    if (!host::read_into(reinterpret_cast<const std::uint8_t*>(record_from),
                         std::span<std::uint8_t>(page.data(), count_bytes)))
        return;
    const std::size_t copied_at = static_cast<std::size_t>(first) * static_cast<std::size_t>(step);
    changed_records(entry.copied.data() + copied_at, page.data(), count_bytes, static_cast<std::size_t>(step),
                    static_cast<std::uint32_t>(first), out);
}

std::optional<std::vector<std::pair<std::size_t, std::size_t>>> guarded_ranges(HeapEntry& entry) {
    std::lock_guard<std::mutex> lock(entry.guard_mutex);
    if (!entry.guard_usable) return std::nullopt;
    const auto start = static_cast<std::size_t>(entry.start);
    const auto end = static_cast<std::size_t>(entry.end);
    const auto pages = watch::writes(start, end, true);
    if (!pages) {
        entry.guard_usable = false;
        return std::nullopt;
    }
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    if (entry.guard_seen == 0) {
        ranges.emplace_back(start, end);
    } else {
        for (const auto& [page, stamp] : pages->written) {
            if (stamp <= entry.guard_seen) continue;
            const std::size_t from = std::max(page * watch::kPageBytes, start);
            const std::size_t to = std::min((page + 1) * watch::kPageBytes, end);
            if (from >= to) continue;
            if (!ranges.empty() && ranges.back().second == from)
                ranges.back().second = to;
            else
                ranges.emplace_back(from, to);
        }
    }
    entry.guard_seen = pages->asked;
    return ranges;
}

void sync_records(const std::shared_ptr<HeapEntry>& entry, std::span<host::HostList* const> lists) {
    if (!entry || entry->step == 0 || entry->count == 0) return;
    if (!entry->watch_probed) {
        entry->watch_probed = true;
        entry->watch_usable = probe_watch(entry->start, entry->end - entry->start);
    }
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    if (entry->watch_usable)
        ranges = take_watch(entry->start, entry->end);
    else if (auto guarded = guarded_ranges(*entry))
        ranges = std::move(*guarded);
    else
        ranges.emplace_back(static_cast<std::size_t>(entry->start), static_cast<std::size_t>(entry->end));
    if (ranges.empty()) return;
    std::vector<std::pair<std::size_t, std::size_t>> parts;
    for (const auto& [from, to] : ranges) {
        std::size_t page = from & ~(kRecordPage - 1);
        while (page < to) {
            parts.emplace_back(std::max(page, from), std::min(page + kRecordPage, to));
            page += kRecordPage;
        }
    }
    if (parts.empty()) return;
    if (entry->host == nullptr && for_heap(entry->heap) == nullptr) return;
    if (entry->host == nullptr) return;
    {
        std::lock_guard<std::mutex> copied(entry->copied_mutex);
        entry->copied.resize(static_cast<std::size_t>(entry->count) * entry->step, 0);
    }
    std::vector<std::vector<std::uint32_t>> found(parts.size());
    const auto look = [&](std::size_t n) { compare_part(*entry, parts[n].first, parts[n].second, found[n]); };
    if (parts.size() <= kSyncPages) {
        for (std::size_t n = 0; n < parts.size(); ++n) look(n);
    } else {
        const std::size_t groups = (parts.size() + kSyncPages - 1) / kSyncPages;
        Workers::shared().run(groups, [&](std::size_t group) {
            for (std::size_t n = group * kSyncPages; n < std::min(parts.size(), (group + 1) * kSyncPages); ++n)
                look(n);
        });
    }
    std::vector<std::uint32_t> make;
    for (const auto& part : found) {
        for (const std::uint32_t index : part) {
            const std::uint64_t handle = entry->start + static_cast<std::uint64_t>(index) * entry->step;
            const auto view = descriptor::view(handle);
            if (tabled_recently(*entry, index) && !(view && shows_other_kind(*entry, index, *view))) continue;
            if (!view && entry->host->shows_none(index)) continue;
            entry->seen.mark(index);
            make.push_back(index);
        }
    }
    if (make.empty()) return;
    materialize_indices(*entry->host, entry->start, entry->step, make);
    std::vector<BufferHeapAlias> refresh;
    apply_heap_data(*entry, make, refresh);
    for (host::HostList* const list : lists) {
        if (list == nullptr) continue;
        refresh_aliases(*list, refresh);
    }
}

void sweep_entry(const std::shared_ptr<HeapEntry>& entry, std::span<host::HostList* const> lists) {
    if (!entry) return;
    if (entry->host) note_evicted(*entry);
    std::vector<std::uint32_t> slots;
    collect_dirty(*entry, slots);
    if (slots.empty()) return;
    if (entry->host == nullptr && for_heap(entry->heap) == nullptr) return;
    if (entry->host == nullptr) return;
    materialize_indices(*entry->host, entry->start, entry->step, slots);
    std::vector<BufferHeapAlias> refresh;
    apply_heap_data(*entry, slots, refresh);
    for (host::HostList* const list : lists) {
        if (list == nullptr) continue;
        refresh_aliases(*list, refresh);
    }
    log_kind_mismatches(*entry);
}

}  // namespace

bool lazy_descriptors() {
    const int cached = g_lazy.load(std::memory_order_relaxed);
    if (cached >= 0) return cached != 0;
    const std::optional<std::string> setting = settings::get("G6SHIM_LAZY_DESCRIPTORS");
    const bool on = !setting || setting->empty() || setting->front() != '0';
    g_lazy.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

void refresh_descriptor_settings() { g_lazy.store(-1, std::memory_order_relaxed); }

void sweep_before_submission(std::span<host::HostList* const> lists) {
    expire_placed_textures();
    std::vector<Object*> wanted;
    for (host::HostList* const list : lists) {
        if (list == nullptr) continue;
        const std::vector<Object*> bound = list->descriptor_heaps();
        wanted.insert(wanted.end(), bound.begin(), bound.end());
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (Object* heap : wanted) {
        const std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
        if (!entry) continue;
        sync_records(entry, lists);
        if (lazy_descriptors()) sweep_entry(entry, lists);
    }
}

void refresh_heap(Object* heap) {
    if (heap == nullptr) return;
    const std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry) return;
    sync_records(entry, {});
    if (lazy_descriptors()) sweep_entry(entry, {});
}

void note_draw_table(Object* heap, std::uint32_t index) {
    const std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry || index >= entry->tabled.size()) return;
    const std::uint32_t now = (static_cast<std::uint32_t>(frame::current_frame()) + 1) & ~kTabledRead;
    entry->tabled[index].store(now | kTabledRead, std::memory_order_relaxed);
}

std::optional<descriptor::View> materialize_for_draw(Object* heap, std::uint32_t index, std::uint64_t handle) {
    const std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry || index >= entry->count) return std::nullopt;
    HostHeap* host = for_heap(heap);
    if (host == nullptr) return std::nullopt;
    return host->mirror(index, handle, true);
}

DescriptorStats take_descriptor_stats() {
    DescriptorStats stats;
    stats.marked = g_marked.exchange(0, std::memory_order_relaxed);
    stats.materialized = g_materialized.exchange(0, std::memory_order_relaxed);
    stats.skipped = g_skipped.exchange(0, std::memory_order_relaxed);
    return stats;
}

void release_retired_views() {
    std::lock_guard<std::mutex> lock(g_retire_mutex);
    ++g_retire_clock;
    std::erase_if(g_retired, [](const RetiredView& entry) { return entry.until <= g_retire_clock; });
}

std::uint64_t retired_view_count() {
    std::lock_guard<std::mutex> lock(g_retire_mutex);
    return g_retired.size();
}

}  // namespace g6shim::d3d12_xs::descriptor_host
