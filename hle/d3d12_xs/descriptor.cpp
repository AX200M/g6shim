// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "descriptor.hpp"

#include "later.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace g6shim::d3d12_xs::descriptor {

namespace {

constexpr std::uint32_t kViewTag = static_cast<std::uint32_t>('v') |
                                   static_cast<std::uint32_t>('V') << 8 |
                                   static_cast<std::uint32_t>('I') << 16 |
                                   static_cast<std::uint32_t>('E') << 24;
constexpr std::uint32_t kRtvTag = static_cast<std::uint32_t>('v') |
                                   static_cast<std::uint32_t>('R') << 8 |
                                   static_cast<std::uint32_t>('T') << 16 |
                                   static_cast<std::uint32_t>('V') << 24;
constexpr std::uint32_t kDsvTag = static_cast<std::uint32_t>('v') |
                                   static_cast<std::uint32_t>('D') << 8 |
                                   static_cast<std::uint32_t>('S') << 16 |
                                   static_cast<std::uint32_t>('V') << 24;
constexpr std::uint64_t kDescriptorAlignment = 64;

struct Entry {
    View value;
    std::uint32_t generation;
};

struct Registry {
    std::vector<Entry> list;
    std::map<View, std::uint32_t> index;
    std::map<std::size_t, std::vector<std::uint32_t>> by_resource;
    std::vector<std::uint32_t> free;
};

std::mutex g_registry_mutex;
Registry g_registry;

Registry& registry() { return g_registry; }
std::mutex& registry_mutex() { return g_registry_mutex; }

std::array<std::size_t, 2> resources(const View& view) {
    if (const auto* shader = std::get_if<ShaderView>(&view)) return {shader->resource, 0};
    if (const auto* unordered = std::get_if<UnorderedView>(&view)) return {unordered->resource, unordered->counter};
    return {0, 0};
}

bool names(const View& view, std::size_t resource) {
    const auto named = resources(view);
    return named[0] == resource || named[1] == resource;
}

template <std::size_t N>
std::optional<std::array<std::uint32_t, N>> words(const std::uint32_t* desc) {
    if (desc == nullptr) return std::nullopt;
    std::array<std::uint32_t, N> out{};
    std::memcpy(out.data(), desc, sizeof out);
    return out;
}

void write_words(std::uint64_t handle, const std::uint32_t* data, std::size_t size) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)), data, size);
}

void record(std::uint64_t handle, const View& view) {
    if (handle == 0) return;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        Registry& all = registry();
        std::uint32_t index = 0;
        const auto existing = all.index.find(view);
        if (existing != all.index.end()) {
            index = existing->second;
        } else if (!all.free.empty()) {
            index = all.free.back();
            all.free.pop_back();
            all.list[index].value = view;
        } else {
            index = static_cast<std::uint32_t>(all.list.size());
            all.list.push_back(Entry{view, 1});
        }
        if (existing == all.index.end()) {
            for (const std::size_t resource : resources(view))
                if (resource != 0) all.by_resource[resource].push_back(index);
            all.index.emplace(view, index);
        }
        const std::uint32_t generation = all.list[index].generation;
        const std::array<std::uint32_t, 3> record_words{kViewTag, index, generation};
        write_words(handle, record_words.data(), sizeof record_words);
    }
    later::descriptor_mirror(handle);
}

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kDescriptorHeap, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_heap(Object* object) { return release_boxed<Heap>(object); }

void heap_desc(Object* object, HeapDesc* out) {
    if (out != nullptr) *out = static_cast<Heap*>(object)->desc;
}

std::uint64_t heap_start(Object* object) { return static_cast<Heap*>(object)->start; }

std::array<const void*, 24> make_functions_impl() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kDescriptorHeap, 24>());
    f[0] = reinterpret_cast<const void*>(&query);
    f[2] = reinterpret_cast<const void*>(&release_heap);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    f[8] = reinterpret_cast<const void*>(&heap_desc);
    f[9] = reinterpret_cast<const void*>(&heap_start);
    f[10] = reinterpret_cast<const void*>(&heap_start);
    return f;
}

std::optional<RtvRecord> tagged_record(std::uint64_t handle, std::uint32_t tag) {
    if (handle == 0) return std::nullopt;
    RtvRecord record{};
    std::memcpy(&record, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(handle)), sizeof record);
    if (record.tag != tag) return std::nullopt;
    return record;
}

}  // namespace

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> table = make_functions_impl();
    return table;
}

Heap::Heap(const HeapDesc& description, std::uint64_t first, std::optional<resource::Memory> storage)
    : Object(table().data()), desc(description), start(first), memory(std::move(storage)) {}

Heap::~Heap() { later::descriptor_heap_dropped(this); }

std::optional<std::uint32_t> Heap::index(std::uint64_t handle) const {
    const auto step = increment(desc.kind);
    if (!step || handle < start) return std::nullopt;
    const std::uint64_t index = (handle - start) / *step;
    if (index >= desc.count) return std::nullopt;
    return static_cast<std::uint32_t>(index);
}

std::optional<std::uint32_t> increment(std::uint32_t kind) {
    switch (kind) {
    case 0: return 32;
    case 1: return 16;
    case 2: return 128;
    case 3: return 128;
    default: return std::nullopt;
    }
}

std::uint32_t increment_size(Object*, std::uint32_t kind) { return increment(kind).value_or(0); }

HRESULT create_heap(Object*, const HeapDesc* desc, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    const auto step = increment(desc->kind);
    if (!step) return E_INVALIDARG;
    const std::uint64_t size = static_cast<std::uint64_t>(*step) * std::max(desc->count, 1u);
    Placement placement{};
    std::memcpy(&placement, reinterpret_cast<const std::uint8_t*>(desc) + sizeof(HeapDesc), sizeof placement);
    const bool placed = placement.address != 0 && placement.size == size;
    G6SHIM_TRACE("d3d12_xs: descriptor heap (kind %u, %u descriptors, flags 0x%X, %s)", desc->kind,
                 desc->count, desc->flags, placed ? "placed" : "own memory");
    std::uint64_t start = 0;
    std::optional<resource::Memory> memory;
    if (placed) {
        start = placement.address;
    } else {
        const HRESULT hr = resource::Memory::zeroed(size, kDescriptorAlignment, &memory);
        if (FAILED(hr)) return hr;
        start = memory->address();
    }
    const bool visible =
        (desc->flags & kShaderVisible) != 0 && (desc->kind == kViewsKind || desc->kind == kSamplersKind);
    auto heap = std::make_unique<Heap>(*desc, start, std::move(memory));
    if (visible) later::descriptor_heap_created(heap.get(), start, start + size, true);
    return hand_out(kDescriptorHeap, std::move(heap), iid, kIids, out);
}

void create_rtv(Object*, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle) {
    if (handle == 0) return;
    RtvRecord record{};
    record.tag = kRtvTag;
    record.described = desc != nullptr ? 1 : 0;
    record.resource = resource_object;
    if (desc != nullptr) std::memcpy(record.desc.data(), desc, sizeof record.desc);
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)), &record, sizeof record);
}

void create_dsv(Object* device, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle) {
    create_rtv(device, resource_object, desc, handle);
    if (handle != 0) write_words(handle, &kDsvTag, sizeof kDsvTag);
}

void create_cbv(Object*, const std::uint32_t* desc, std::uint64_t handle) {
    const auto values = words<4>(desc);
    if (!values) return;
    const std::uint64_t address = static_cast<std::uint64_t>((*values)[0]) |
                                  static_cast<std::uint64_t>((*values)[1]) << 32;
    record(handle, ConstantView{address, (*values)[2]});
}

void create_srv(Object*, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle) {
    record(handle, ShaderView{reinterpret_cast<std::uintptr_t>(resource_object), words<10>(desc)});
}

void create_uav(Object*, Object* resource_object, Object* counter, const std::uint32_t* desc, std::uint64_t handle) {
    record(handle, UnorderedView{reinterpret_cast<std::uintptr_t>(resource_object),
                                 reinterpret_cast<std::uintptr_t>(counter), words<10>(desc)});
}

void create_placed_srv(Object*, const std::uint32_t* resource_desc, const std::uint32_t* desc, std::uint64_t handle) {
    const auto resource_words = words<14>(resource_desc);
    const auto view_words = words<16>(desc);
    if (resource_words && view_words) record(handle, PlacedView{false, *resource_words, *view_words});
}

void create_placed_uav(Object*, const std::uint32_t* resource_desc, const std::uint32_t* desc, std::uint64_t handle) {
    const auto resource_words = words<14>(resource_desc);
    const auto view_words = words<16>(desc);
    if (resource_words && view_words) record(handle, PlacedView{true, *resource_words, *view_words});
}

void create_sampler(Object*, const std::uint32_t* desc, std::uint64_t handle) {
    const auto values = words<13>(desc);
    if (values) record(handle, SamplerView{*values});
}

void create_sampler_x(Object*, const std::uint32_t* desc, std::uint64_t handle) {
    const auto values = words<27>(desc);
    if (values) record(handle, ConsoleSamplerView{*values});
}

void copy_simple(Object*, std::uint32_t count, std::uint64_t destination, std::uint64_t source, std::uint32_t kind) {
    const std::uint64_t step = increment(kind).value_or(0);
    if (step == 0) return;
    const std::uint64_t length = static_cast<std::uint64_t>(count) * step;
    if (destination == 0 || source == 0) return;
    std::memmove(reinterpret_cast<void*>(static_cast<std::uintptr_t>(destination)),
                 reinterpret_cast<const void*>(static_cast<std::uintptr_t>(source)),
                 static_cast<std::size_t>(length));
    for (std::uint64_t i = 0; i < count; ++i) later::descriptor_mirror(destination + i * step);
}

void copy(Object*, std::uint32_t destination_ranges, const std::uint64_t* destination_starts,
          const std::uint32_t* destination_sizes, std::uint32_t source_ranges, const std::uint64_t* source_starts,
          const std::uint32_t* source_sizes, std::uint32_t kind) {
    const auto step = increment(kind);
    if (!step || destination_starts == nullptr || source_starts == nullptr) return;
    const auto ranges = [](std::uint32_t count, const std::uint64_t* starts, const std::uint32_t* sizes) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> out;
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            out.emplace_back(starts[i], sizes != nullptr ? sizes[i] : 1u);
        return out;
    };
    const auto source_list = ranges(source_ranges, source_starts, source_sizes);
    const auto destination_list = ranges(destination_ranges, destination_starts, destination_sizes);
    auto next = destination_list.begin();
    std::uint64_t to = 0;
    std::uint32_t room = 0;
    if (next != destination_list.end()) {
        to = next->first;
        room = next->second;
        ++next;
    }
    for (const auto& [source_start, source_count] : source_list) {
        std::uint64_t from = source_start;
        std::uint32_t left = source_count;
        while (left > 0) {
            while (room == 0) {
                if (next == destination_list.end()) return;
                to = next->first;
                room = next->second;
                ++next;
            }
            const std::uint32_t n = std::min(left, room);
            copy_simple(nullptr, n, to, from, kind);
            const std::uint64_t bytes = static_cast<std::uint64_t>(n) * *step;
            to += bytes;
            from += bytes;
            room -= n;
            left -= n;
        }
    }
}

void forget(Object* resource_object) {
    if (resource_object == nullptr) return;
    const auto key = reinterpret_cast<std::uintptr_t>(resource_object);
    std::lock_guard<std::mutex> lock(registry_mutex());
    Registry& all = registry();
    const auto entries = all.by_resource.find(key);
    if (entries == all.by_resource.end()) return;
    const std::vector<std::uint32_t> indices = entries->second;
    all.by_resource.erase(entries);
    for (const std::uint32_t index : indices) {
        const View value = all.list[index].value;
        const auto current = all.index.find(value);
        if (current == all.index.end() || current->second != index || !names(value, key)) continue;
        all.index.erase(current);
        std::uint32_t generation = all.list[index].generation + 1;
        if (generation == 0) generation = 1;
        all.list[index].generation = generation;
        all.free.push_back(index);
    }
}

std::optional<View> view(std::uint64_t handle) {
    if (handle == 0) return std::nullopt;
    std::array<std::uint32_t, 3> record_words{};
    std::memcpy(record_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(handle)),
                sizeof record_words);
    std::lock_guard<std::mutex> lock(registry_mutex());
    Registry& all = registry();
    if (record_words[1] >= all.list.size()) return std::nullopt;
    const Entry& entry = all.list[record_words[1]];
    if (record_words[0] != kViewTag || record_words[2] != entry.generation) return std::nullopt;
    return entry.value;
}

std::optional<RtvRecord> rtv_record(std::uint64_t handle) { return tagged_record(handle, kRtvTag); }

std::optional<RtvRecord> dsv_record(std::uint64_t handle) { return tagged_record(handle, kDsvTag); }

namespace {

template <typename T>
std::optional<T> host_description(const RtvRecord& record, std::uint32_t dimension) {
    static_assert(sizeof(T) <= sizeof(record.desc));
    std::array<std::uint32_t, 6> words{};
    if (record.described != 0) {
        words = record.desc;
    } else {
        const auto format = resource::view_format(record.resource);
        if (!format) return std::nullopt;
        words = {*format, dimension, 0, 0, 0, 0};
    }
    words[0] = resource::pc_format(words[0]);
    T description{};
    std::memcpy(&description, words.data(), sizeof description);
    return description;
}

}  // namespace

std::optional<D3D12_RENDER_TARGET_VIEW_DESC> rtv_description(const RtvRecord& record) {
    constexpr std::uint32_t kTexture2D = 4;
    return host_description<D3D12_RENDER_TARGET_VIEW_DESC>(record, kTexture2D);
}

std::optional<D3D12_DEPTH_STENCIL_VIEW_DESC> dsv_description(const RtvRecord& record) {
    constexpr std::uint32_t kTexture2D = 3;
    return host_description<D3D12_DEPTH_STENCIL_VIEW_DESC>(record, kTexture2D);
}
void release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count) {
    later::descriptor_release_dynamic(heap, first, count);
}

}  // namespace g6shim::d3d12_xs::descriptor
