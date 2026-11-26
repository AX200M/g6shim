// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture.hpp"

#include "watch.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace g6shim::d3d12_xs::texture {

namespace {

constexpr std::uint32_t kPcFlags = 0xf;
constexpr std::uint32_t kDepth = 2;
constexpr std::uint32_t kUnordered = 4;
constexpr std::uint32_t kDenyShaderResource = 8;
constexpr std::uint32_t kTargetDepthUnordered = 0x7;
constexpr std::uint32_t kCopyDest = 0x400;
constexpr std::size_t kClearWords = 5;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::mutex g_staging_mutex;
std::unordered_map<std::uintptr_t, std::shared_ptr<Staging>> g_staging;

std::mutex g_written_mutex;
std::unordered_set<std::uintptr_t> g_written;

std::uint64_t content_hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = kFnvOffset;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

std::shared_ptr<later::HostTexture> create_host_texture(const resource::ResourceDesc& desc, std::uint32_t state,
                                                        const void* clear) {
    if (desc.dimension == resource::kBuffer) return nullptr;
    host::Host* gpu = host::host();
    if (gpu == nullptr) return nullptr;
    resource::ResourceDesc host_desc = desc;
    host_desc.layout = 0;
    host_desc.alignment = 0;
    host_desc.flags &= kPcFlags;
    host_desc.format = resource::pc_format(host_desc.format);
    if ((host_desc.flags & kDenyShaderResource) == 0) {
        const auto depth = std::find_if(resource::kDepthFormats.begin(), resource::kDepthFormats.end(),
                                        [&](const auto& pair) { return pair.first == host_desc.format; });
        if (depth != resource::kDepthFormats.end()) host_desc.format = depth->second;
    }
    const bool both = (host_desc.flags & kDepth) != 0 && (host_desc.flags & kUnordered) != 0;
    bool direct = both && depth_unordered_direct(host_desc.format, host_desc.sample_count);
    if ((host_desc.flags & kDepth) != 0 && !direct) host_desc.flags &= ~kUnordered;
    if ((host_desc.flags & kUnordered) != 0) {
        const auto srgb = std::find_if(resource::kSrgbFormats.begin(), resource::kSrgbFormats.end(),
                                       [&](const auto& pair) { return pair.first == host_desc.format; });
        if (srgb != resource::kSrgbFormats.end()) host_desc.format = resource::family(host_desc.format);
    }
    std::array<std::uint32_t, kClearWords> clear_value{};
    const bool has_clear = clear != nullptr;
    if (has_clear) {
        std::memcpy(clear_value.data(), clear, sizeof clear_value);
        clear_value[0] = resource::pc_format(clear_value[0]);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    std::uint32_t initial = host::legal_for(host_desc.flags, host::texture_state(state));
    const void* clear_pointer = has_clear ? static_cast<const void*>(clear_value.data()) : nullptr;
    auto create = [&]() {
        return gpu->device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, reinterpret_cast<const D3D12_RESOURCE_DESC*>(&host_desc),
            static_cast<D3D12_RESOURCE_STATES>(initial), static_cast<const D3D12_CLEAR_VALUE*>(clear_pointer),
            IID_PPV_ARGS(&texture));
    };
    HRESULT made = create();
    if (FAILED(made) && direct) {
        host_desc.flags &= ~kUnordered;
        direct = false;
        initial = host::legal_for(host_desc.flags, host::texture_state(state));
        made = create();
    }
    if (FAILED(made)) {
        G6SHIM_WARN("d3d12_xs: no host texture (dimension %u, format %u, 0x%08lX)", host_desc.dimension,
                    host_desc.format, static_cast<unsigned long>(made));
        return nullptr;
    }
    char label[64] = {};
    std::snprintf(label, sizeof label, "texture %llux%u f%u flags 0x%x",
                  static_cast<unsigned long long>(host_desc.width), host_desc.height, host_desc.format,
                  host_desc.flags);
    host::name(texture.Get(), label);
    auto host_texture = std::make_shared<later::HostTexture>();
    host_texture->texture = std::move(texture);
    host_texture->state.store(initial, std::memory_order_relaxed);
    if (both) attach_depth_uav(*host_texture, host_desc, direct, clear);
    return host_texture;
}

ID3D12Resource* host_texture(Object* object) {
    const std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    return copy ? copy->texture.Get() : nullptr;
}

std::optional<std::uint32_t> host_texture_format(Object* object) {
    ID3D12Resource* texture = host_texture(object);
    if (texture == nullptr) return std::nullopt;
    return static_cast<std::uint32_t>(texture->GetDesc().Format);
}

std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* object, std::uint32_t wanted) {
    std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (!copy) return std::nullopt;
    const std::uint32_t before = copy->state.exchange(wanted, std::memory_order_relaxed);
    if (before == wanted) return std::nullopt;
    return std::make_pair(static_cast<void*>(copy->texture.Get()), before);
}

void forget(Object* object) {
    if (object == nullptr) return;
    const auto key = reinterpret_cast<std::uintptr_t>(object);
    {
        std::lock_guard<std::mutex> lock(g_staging_mutex);
        g_staging.erase(key);
    }
    {
        std::lock_guard<std::mutex> lock(g_written_mutex);
        g_written.erase(key);
    }
}

void host_written(Object* object) {
    if (object == nullptr) return;
    std::lock_guard<std::mutex> lock(g_written_mutex);
    g_written.insert(reinterpret_cast<std::uintptr_t>(object));
}

bool committed(const void* at, std::size_t bytes) {
    std::size_t address = reinterpret_cast<std::size_t>(at);
    if (bytes > std::numeric_limits<std::size_t>::max() - address) return false;
    const std::size_t end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) == 0) return false;
        if (info.State != MEM_COMMIT) return false;
        address = reinterpret_cast<std::size_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool title_written(ID3D12Resource* texture) {
    if (texture == nullptr) return false;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    return (static_cast<std::uint32_t>(desc.Flags) & kTargetDepthUnordered) == 0;
}

Staging::Staging(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::uint8_t* data,
                 std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints, std::vector<std::uint32_t> rows,
                 gfx10::Layout layout, std::uint32_t mips, const std::uint8_t* memory)
    : buffer_(std::move(buffer)),
      data_(data),
      footprints_(std::move(footprints)),
      rows_(std::move(rows)),
      layout_(std::move(layout)),
      mips_(mips),
      memory_(memory) {}

void Staging::refresh() { refresh_now(); }

std::uint64_t Staging::refresh_now() {
    const std::size_t size = static_cast<std::size_t>(layout_.size);
    const std::size_t start = reinterpret_cast<std::size_t>(memory_);
    std::lock_guard<std::mutex> lock(last_mutex_);
    if (const auto pages = watch::writes(start, start + size, false)) {
        if (last_.asked != 0) {
            const bool unchanged = std::all_of(pages->written.begin(), pages->written.end(),
                                               [this](const auto& page) { return page.second <= last_.asked; });
            if (unchanged) return 0;
        }
        last_.asked = pages->asked;
        last_.read.resize(size, 0);
        host::copy_mapped(last_.read.data(), memory_, size, false);
        detile(last_.read);
        return size;
    }
    if (!committed(memory_, size)) return 0;
    const std::uint64_t hash = content_hash(std::span<const std::uint8_t>(memory_, size));
    if (last_.hash == hash) return size;
    last_.hash = hash;
    detile(std::span<const std::uint8_t>(memory_, size));
    return size;
}

void Staging::detile(std::span<const std::uint8_t> tiled) {
    for (std::size_t i = 0; i < footprints_.size(); ++i) {
        const std::uint32_t mip = static_cast<std::uint32_t>(i) % mips_;
        const std::uint32_t slice = static_cast<std::uint32_t>(i) / mips_;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints_[i];
        const D3D12_SUBRESOURCE_FOOTPRINT& part = footprint.Footprint;
        const std::size_t pitch = part.RowPitch;
        const std::size_t rows = rows_[i];
        const std::size_t bytes = pitch * rows * part.Depth;
        std::span<std::uint8_t> linear(data_ + footprint.Offset, bytes);
        const gfx10::Error error = layout_.detile(tiled, mip, slice, linear, pitch, pitch * rows);
        if (error != gfx10::Error::kOk) {
            G6SHIM_WARN("d3d12_xs: texture not detiled (mip %u, slice %u, error %d)", mip, slice,
                        static_cast<int>(error));
            return;
        }
    }
}

std::optional<std::shared_ptr<Staging>> staging(Object* object, ID3D12Resource* texture) {
    if (object == nullptr || texture == nullptr) return std::nullopt;
    const auto key = reinterpret_cast<std::uintptr_t>(object);
    {
        std::lock_guard<std::mutex> lock(g_staging_mutex);
        const auto found = g_staging.find(key);
        if (found != g_staging.end()) return found->second;
    }
    const auto memory = resource::texture_memory(object);
    if (!memory) return std::nullopt;
    const auto layout_texture = resource::texture_layout(memory->second);
    if (!layout_texture) {
        G6SHIM_WARN("d3d12_xs: texture contents without a console layout not uploaded");
        return std::nullopt;
    }
    const gfx10::Layout& layout = layout_texture->planes[0].layout;
    if (gfx10::is_xor(layout.desc.swizzle)) {
        G6SHIM_WARN("d3d12_xs: texture contents in an XOR mode not uploaded");
        return std::nullopt;
    }
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const std::uint32_t count = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                    ? static_cast<std::uint32_t>(desc.MipLevels)
                                    : static_cast<std::uint32_t>(desc.MipLevels) * desc.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<std::uint32_t> rows(count, 0);
    std::uint64_t total = 0;
    gpu->device()->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), nullptr, &total);
    auto buffer = gpu->upload_buffer(total);
    if (!buffer) return std::nullopt;
    char label[96] = {};
    std::snprintf(label, sizeof label, "staging of %llux%u f%u",
                  static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned int>(desc.Format));
    host::name(buffer->first.Get(), label);
    G6SHIM_TRACE("d3d12_xs: texture contents uploaded from title memory (%llu bytes)",
                 static_cast<unsigned long long>(total));
    auto made = std::make_shared<Staging>(std::move(buffer->first), buffer->second, std::move(footprints),
                                          std::move(rows), layout, static_cast<std::uint32_t>(desc.MipLevels),
                                          memory->first);
    std::lock_guard<std::mutex> lock(g_staging_mutex);
    g_staging.insert_or_assign(key, made);
    return made;
}

void upload(host::HostList& list, Object* object, ID3D12Resource* texture) {
    if (object == nullptr || texture == nullptr) return;
    const auto key = reinterpret_cast<std::uintptr_t>(object);
    {
        std::lock_guard<std::mutex> lock(g_written_mutex);
        if (g_written.find(key) != g_written.end()) return;
    }
    if (!list.first_use(key)) return;
    auto staged = staging(object, texture);
    if (!staged) return;
    host::ensure_state(list, object, kCopyDest);
    const auto& footprints = (*staged)->footprints();
    for (std::size_t i = 0; i < footprints.size(); ++i) {
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = texture;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = static_cast<UINT>(i);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = (*staged)->buffer();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprints[i];
        list.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    list.defer_texture(*staged);
}

}  // namespace g6shim::d3d12_xs::texture
