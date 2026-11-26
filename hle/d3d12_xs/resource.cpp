// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "resource.hpp"

#include "descriptor.hpp"
#include "host.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <variant>
#include <vector>

namespace g6shim::d3d12_xs::resource {

namespace {

constexpr std::uint64_t kGraphicsAllocator = 128 << 16;
constexpr std::uint32_t kDefaultHeap = 1;
constexpr std::uint32_t kCustomHeap = 4;
constexpr std::uint32_t kNotAvailable = 1;
constexpr std::uint32_t kWriteBack = 3;
constexpr std::uint32_t kPoolL0 = 1;
constexpr std::uint64_t kPlacement = 512;
constexpr std::uint64_t kPitch = 256;
constexpr std::uint32_t kTexture1d = 2;
constexpr std::uint32_t kTexture2d = 3;
constexpr std::uint32_t kRenderTarget = 1;
constexpr std::uint32_t kDepth = 2;
constexpr std::uint32_t kLayoutExplicit = 0x100;
constexpr std::uint32_t kSwizzle64kbZ = 24;
constexpr std::uint32_t kSwizzle64kbR = 27;
constexpr std::uint32_t kSwizzle4kbS = 5;
constexpr std::uint32_t kSwizzleLinear = 0;

constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 22> kFamilies{{
    {1, 4},
    {5, 8},
    {9, 14},
    {15, 18},
    {19, 22},
    {23, 25},
    {27, 32},
    {33, 38},
    {39, 43},
    {44, 47},
    {48, 52},
    {53, 59},
    {60, 65},
    {70, 72},
    {73, 75},
    {76, 78},
    {79, 81},
    {82, 84},
    {90, 91},
    {92, 93},
    {94, 96},
    {97, 99},
}};

struct Allocator {
    AllocRoutine allocate = nullptr;
    FreeRoutine free = nullptr;
};

std::mutex g_allocator_mutex;
Allocator g_allocator;
std::mutex g_mapper_mutex;
PageMapper g_mapper = nullptr;

std::optional<PageMapper> locked_mapper() {
    std::lock_guard<std::mutex> lock(g_mapper_mutex);
    if (g_mapper == nullptr) return std::nullopt;
    return g_mapper;
}

Allocator allocator() {
    std::lock_guard<std::mutex> lock(g_allocator_mutex);
    return g_allocator;
}

std::optional<std::uint64_t> round_up(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0) return std::nullopt;
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0) return value;
    if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - remainder)) return std::nullopt;
    return value + (alignment - remainder);
}

std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return std::nullopt;
    return a + b;
}

std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return std::nullopt;
    return a * b;
}

bool valid_layout(std::uint64_t size, std::uint64_t alignment) {
    if (size == 0 || alignment == 0 || !std::has_single_bit(alignment)) return false;
    constexpr std::uint64_t kMaximum = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    return size <= kMaximum - (alignment - 1);
}

std::uint64_t resource_attributes(const AllocationInfo& info, std::uint32_t heap_kind) {
    const std::uint64_t pages = info.size >= (2 << 20) ? 2 : 1;
    const std::uint64_t memory = heap_kind == kDefaultHeap ? 1 : 3;
    const std::uint64_t alignment = std::countr_zero(std::max<std::uint64_t>(info.alignment, 1));
    return kGraphicsAllocator | pages << 14 | alignment << 24 | memory << 29;
}

struct Subresource {
    PlacedFootprint footprint;
    std::uint32_t rows;
    std::uint64_t row_size;
};

std::optional<Subresource> subresource(const ResourceDesc& desc, std::uint32_t index) {
    if (desc.dimension == kBuffer) {
        if (index != 0 || desc.width > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        const std::uint64_t row_pitch = *round_up(desc.width, kPitch);
        if (row_pitch > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        PlacedFootprint footprint{};
        footprint.width = static_cast<std::uint32_t>(desc.width);
        footprint.height = 1;
        footprint.depth = 1;
        footprint.row_pitch = static_cast<std::uint32_t>(row_pitch);
        return Subresource{footprint, 1, desc.width};
    }
    const std::uint32_t mips = mip_count(desc);
    const std::uint32_t slices =
        desc.dimension == kTexture3d ? 1 : std::max<std::uint32_t>(desc.depth_or_array_size, 1);
    const std::uint32_t mip = index % mips;
    const std::uint32_t slice = index / mips;
    if (slice >= slices) return std::nullopt;
    const auto format = element(desc.format);
    if (!format) return std::nullopt;
    const std::uint64_t element_bytes = format->first;
    const std::uint64_t block = format->second;
    const std::optional<std::uint64_t> width =
        round_up(std::max<std::uint64_t>(desc.width >> mip, 1), block);
    const std::optional<std::uint64_t> height =
        round_up(std::max<std::uint64_t>(desc.height >> mip, 1), block);
    if (!width || !height) return std::nullopt;
    const std::uint32_t depth =
        desc.dimension == kTexture3d ? std::max<std::uint32_t>(desc.depth_or_array_size >> mip, 1) : 1;
    const std::uint64_t row_size = *width / block * element_bytes;
    const std::optional<std::uint64_t> row_pitch = round_up(row_size, kPitch);
    if (!row_pitch || *width > std::numeric_limits<std::uint32_t>::max() ||
        *height > std::numeric_limits<std::uint32_t>::max() || *row_pitch > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    PlacedFootprint footprint{};
    footprint.format = desc.format;
    footprint.width = static_cast<std::uint32_t>(*width);
    footprint.height = static_cast<std::uint32_t>(*height);
    footprint.depth = depth;
    footprint.row_pitch = static_cast<std::uint32_t>(*row_pitch);
    return Subresource{footprint, static_cast<std::uint32_t>(*height / block), row_size};
}

constexpr PlacedFootprint kNoFootprint{std::numeric_limits<std::uint64_t>::max(), 0, 0, 0, 0, 0};

template <typename Each>
AllocationInfo place(std::span<const ResourceDesc> descs, Each each) {
    AllocationInfo total{0, kKib64};
    for (std::size_t i = 0; i < descs.size(); ++i) {
        const auto one = footprint(descs[i]);
        if (!one) {
            G6SHIM_WARN("d3d12_xs: no size for resource %zu (dimension %u, format %u)", i, descs[i].dimension,
                        descs[i].format);
            each(i, AllocationInfo1{std::numeric_limits<std::uint64_t>::max(), 0,
                                    std::numeric_limits<std::uint64_t>::max()});
            return AllocationInfo{std::numeric_limits<std::uint64_t>::max(), 0};
        }
        const auto offset = round_up(total.size, one->alignment);
        if (!offset) {
            each(i, AllocationInfo1{std::numeric_limits<std::uint64_t>::max(), 0,
                                    std::numeric_limits<std::uint64_t>::max()});
            return AllocationInfo{std::numeric_limits<std::uint64_t>::max(), 0};
        }
        each(i, AllocationInfo1{*offset, one->alignment, one->size});
        const auto end = checked_add(*offset, one->size);
        if (!end) return AllocationInfo{std::numeric_limits<std::uint64_t>::max(), 0};
        total.size = *end;
        total.alignment = std::max(total.alignment, one->alignment);
    }
    return total;
}

HRESULT query_heap(Object* object, const Iid* iid, Object** out) {
    return answer(kHeap, iid, is_iid(iid, kHeapIids) ? object : nullptr, out);
}

std::uint32_t release_heap(Object* object) { return release_boxed<Heap>(object); }

void heap_desc(Object* object, HeapDesc* out) {
    if (out != nullptr) std::memcpy(out, &static_cast<Heap*>(object)->desc, sizeof *out);
}

std::array<const void*, 24> make_heap_functions_impl() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kHeap, 24>());
    f[0] = reinterpret_cast<const void*>(&query_heap);
    f[2] = reinterpret_cast<const void*>(&release_heap);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    f[8] = reinterpret_cast<const void*>(&heap_desc);
    return f;
}

const std::array<const void*, 24>& heap_functions() {
    static const std::array<const void*, 24> table = make_heap_functions_impl();
    return table;
}

struct PlacedBacking {
    Object* heap;
    std::uint64_t offset;
};

const std::array<const void*, 32>& resource_functions();

std::array<const void*, 32> make_resource_functions_impl();

using Backing = std::variant<Memory, PlacedBacking, std::uint8_t*>;

struct Resource : Object {
    Resource(const ResourceDesc& description, const HeapProperties& properties, std::uint32_t properties_flags,
             Backing backing_store, std::shared_ptr<later::HostTexture> host_copy)
        : Object(resource_functions().data()),
          desc(description),
          heap(properties),
          heap_flags(properties_flags),
          backing(std::move(backing_store)),
          host(std::move(host_copy)) {}

    ~Resource() {
        if (auto* placed = std::get_if<PlacedBacking>(&backing)) release_heap(placed->heap);
        descriptor::forget(this);
        later::texture_forget(this);
        if (desc.dimension == kBuffer) later::shadow_unregister(this, data());
    }

    std::uint8_t* data() const {
        if (const auto* memory = std::get_if<Memory>(&backing)) return memory->data();
        if (const auto* placed = std::get_if<PlacedBacking>(&backing))
            return static_cast<Heap*>(placed->heap)->memory.data() + placed->offset;
        return *std::get_if<std::uint8_t*>(&backing);
    }

    ResourceDesc desc;
    HeapProperties heap;
    std::uint32_t heap_flags;
    Backing backing;
    std::shared_ptr<later::HostTexture> host;
};

Resource* resource_of(Object* object) { return static_cast<Resource*>(object); }

const std::array<const void*, 32>& resource_functions() {
    static const std::array<const void*, 32> table = make_resource_functions_impl();
    return table;
}

HRESULT hand_out_resource(const ResourceDesc& desc, const HeapProperties& heap, std::uint32_t heap_flags,
                          Backing backing, std::uint32_t state, const void* clear, const Iid* iid, Object** out) {
    auto host = later::create_host_texture(desc, state, clear);
    auto resource = std::make_unique<Resource>(desc, heap, heap_flags, std::move(backing), std::move(host));
    if (desc.dimension == kBuffer) later::shadow_register(resource.get(), resource->data(), desc.width);
    return hand_out(kResource, std::move(resource), iid, kResourceIids, out);
}

HRESULT resource_query(Object* object, const Iid* iid, Object** out) {
    return answer(kResource, iid, is_iid(iid, kResourceIids) ? object : nullptr, out);
}

std::uint32_t resource_release(Object* object) { return release_boxed<Resource>(object); }

HRESULT map(Object* object, std::uint32_t, const void*, std::uint8_t** out) {
    Resource* resource = resource_of(object);
    if (resource != nullptr && resource->desc.dimension == kBuffer && resource->data() != nullptr)
        host::wait_for_writes(resource->data(), static_cast<std::size_t>(resource->desc.width));
    if (out != nullptr) *out = resource != nullptr ? resource->data() : nullptr;
    return S_OK;
}

void unmap(Object*, std::uint32_t, const void*) {}

void resource_desc(Object* object, ResourceDesc* out) {
    if (out != nullptr) std::memcpy(out, &resource_of(object)->desc, sizeof *out);
}

std::uint64_t gpu_address(Object* object) { return reinterpret_cast<std::uint64_t>(resource_of(object)->data()); }

HRESULT heap_properties(Object* object, HeapProperties* properties, std::uint32_t* flags) {
    const Resource* resource = resource_of(object);
    if (properties != nullptr) *properties = resource->heap;
    if (flags != nullptr) *flags = resource->heap_flags;
    return S_OK;
}

std::array<const void*, 32> make_resource_functions_impl() {
    std::array<const void*, 32> f = d3d12_object(missing_table<kResource, 32>());
    f[0] = reinterpret_cast<const void*>(&resource_query);
    f[2] = reinterpret_cast<const void*>(&resource_release);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    f[8] = reinterpret_cast<const void*>(&map);
    f[9] = reinterpret_cast<const void*>(&unmap);
    f[10] = reinterpret_cast<const void*>(&resource_desc);
    f[11] = reinterpret_cast<const void*>(&gpu_address);
    f[14] = reinterpret_cast<const void*>(&heap_properties);
    return f;
}

std::mutex g_pools_mutex;
std::vector<std::optional<std::pair<std::uint64_t, std::uint32_t>>> g_pools;

}  // namespace

const std::array<const void*, 24>& table() { return heap_functions(); }

void use_allocator(AllocRoutine allocate, FreeRoutine free) {
    if (allocate == nullptr || free == nullptr) return;
    std::lock_guard<std::mutex> lock(g_allocator_mutex);
    if (g_allocator.allocate == nullptr) g_allocator = Allocator{allocate, free};
}

void use_page_mapper(PageMapper mapper) {
    std::lock_guard<std::mutex> lock(g_mapper_mutex);
    if (g_mapper == nullptr) g_mapper = mapper;
}

std::optional<PageMapper> page_mapper() { return locked_mapper(); }

Memory::Memory(std::uint8_t* pointer, std::uint64_t size, std::uint64_t alignment,
               std::optional<std::uint64_t> attributes) noexcept
    : pointer_(pointer), size_(size), alignment_(alignment), attributes_(attributes) {}

Memory::Memory(Memory&& other) noexcept
    : pointer_(other.pointer_),
      size_(other.size_),
      alignment_(other.alignment_),
      attributes_(other.attributes_) {
    other.pointer_ = nullptr;
    other.size_ = 0;
    other.attributes_.reset();
}

Memory& Memory::operator=(Memory&& other) noexcept {
    if (this == &other) return *this;
    reset();
    pointer_ = other.pointer_;
    size_ = other.size_;
    alignment_ = other.alignment_;
    attributes_ = other.attributes_;
    other.pointer_ = nullptr;
    other.size_ = 0;
    other.attributes_.reset();
    return *this;
}

Memory::~Memory() { reset(); }

void Memory::reset() noexcept {
    if (pointer_ == nullptr) return;
    if (attributes_) {
        const Allocator state = allocator();
        if (state.free != nullptr) {
            state.free(pointer_, *attributes_);
            pointer_ = nullptr;
            size_ = 0;
            attributes_.reset();
            return;
        }
    }
    _aligned_free(pointer_);
    pointer_ = nullptr;
    size_ = 0;
    attributes_.reset();
}

HRESULT Memory::create(AllocationInfo info, std::optional<Memory>* out) {
    if (out == nullptr) return E_INVALIDARG;
    if (!valid_layout(info.size, info.alignment)) return E_INVALIDARG;
    void* pointer = _aligned_malloc(static_cast<std::size_t>(info.size), static_cast<std::size_t>(info.alignment));
    if (pointer == nullptr) return E_OUTOFMEMORY;
    std::memset(pointer, 0, static_cast<std::size_t>(info.size));
    *out = Memory(static_cast<std::uint8_t*>(pointer), info.size, info.alignment, std::nullopt);
    return S_OK;
}

HRESULT Memory::zeroed(std::uint64_t size, std::uint64_t alignment, std::optional<Memory>* out) {
    return create(AllocationInfo{size, alignment}, out);
}

HRESULT Memory::for_resource(AllocationInfo info, std::uint32_t heap_kind, std::optional<Memory>* out) {
    const Allocator state = allocator();
    if (state.allocate == nullptr) return create(info, out);
    if (out == nullptr || !valid_layout(info.size, info.alignment)) return E_INVALIDARG;
    const std::uint64_t attributes = resource_attributes(info, heap_kind);
    void* pointer = state.allocate(static_cast<std::size_t>(info.size), attributes);
    if (pointer == nullptr) return E_OUTOFMEMORY;
    std::memset(pointer, 0, static_cast<std::size_t>(info.size));
    *out = Memory(static_cast<std::uint8_t*>(pointer), info.size, info.alignment, attributes);
    return S_OK;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> element(std::uint32_t format) {
    if (format >= 1 && format <= 4) return std::pair<std::uint64_t, std::uint64_t>{16, 1};
    if (format >= 5 && format <= 8) return std::pair<std::uint64_t, std::uint64_t>{12, 1};
    if (format >= 9 && format <= 22) return std::pair<std::uint64_t, std::uint64_t>{8, 1};
    if ((format >= 23 && format <= 47) || format == 67 || (format >= 87 && format <= 93) || format == 116 ||
        format == 117 || format == 189 || format == 191)
        return std::pair<std::uint64_t, std::uint64_t>{4, 1};
    if (format >= 118 && format <= 120) return std::pair<std::uint64_t, std::uint64_t>{3, 1};
    if ((format >= 48 && format <= 59) || format == 68 || format == 69 || format == 85 || format == 86 ||
        format == 115)
        return std::pair<std::uint64_t, std::uint64_t>{2, 1};
    if ((format >= 60 && format <= 65) || format == 190) return std::pair<std::uint64_t, std::uint64_t>{1, 1};
    if ((format >= 70 && format <= 72) || (format >= 79 && format <= 81))
        return std::pair<std::uint64_t, std::uint64_t>{8, 4};
    if ((format >= 73 && format <= 78) || (format >= 82 && format <= 84) || (format >= 94 && format <= 99))
        return std::pair<std::uint64_t, std::uint64_t>{16, 4};
    return std::nullopt;
}

std::optional<gfx10::Texture> texture_layout(const ResourceDesc& desc) {
    gfx10::Dimension dimension = gfx10::Dimension::kTex2d;
    if (desc.dimension == kTexture1d)
        dimension = gfx10::Dimension::kTex1d;
    else if (desc.dimension == kTexture2d)
        dimension = gfx10::Dimension::kTex2d;
    else if (desc.dimension == kTexture3d)
        dimension = gfx10::Dimension::kTex3d;
    else
        return std::nullopt;
    std::uint32_t swizzle = kSwizzleLinear;
    if (desc.layout == 0) {
        if (dimension == gfx10::Dimension::kTex1d)
            swizzle = kSwizzleLinear;
        else if ((desc.flags & kDepth) != 0)
            swizzle = kSwizzle64kbZ;
        else if ((desc.flags & kRenderTarget) != 0)
            swizzle = kSwizzle64kbR;
        else
            swizzle = kSwizzle4kbS;
    } else if (desc.layout == 1) {
        swizzle = kSwizzleLinear;
    } else if ((desc.layout & ~static_cast<std::uint32_t>(0xFF)) == kLayoutExplicit) {
        swizzle = desc.layout & 0xFF;
    } else {
        return std::nullopt;
    }
    if (desc.width > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    gfx10::TextureDesc texture{};
    texture.dimension = dimension;
    texture.width = static_cast<std::uint32_t>(desc.width);
    texture.height = desc.height;
    texture.depth_or_array = desc.depth_or_array_size;
    texture.mips = desc.mip_levels;
    texture.format = desc.format;
    texture.samples = std::max(desc.sample_count, 1u);
    texture.swizzle = swizzle;
    return gfx10::Texture::create(texture);
}

std::optional<AllocationInfo> footprint(const ResourceDesc& desc) {
    if (desc.dimension != kBuffer) {
        if (const auto texture = texture_layout(desc)) {
            const std::uint64_t alignment = std::max(desc.alignment, texture->alignment);
            if (!std::has_single_bit(alignment)) return std::nullopt;
            const auto size = round_up(texture->size, alignment);
            if (!size) return std::nullopt;
            return AllocationInfo{*size, alignment};
        }
    }
    std::uint64_t bytes = 0;
    if (desc.dimension == kBuffer) {
        bytes = desc.width;
    } else {
        const auto format = element(desc.format);
        if (!format) return std::nullopt;
        const std::uint64_t element_bytes = format->first;
        const std::uint64_t block = format->second;
        const std::uint64_t depth = desc.dimension == kTexture3d ? desc.depth_or_array_size : 1;
        const std::uint64_t slices = desc.dimension == kTexture3d ? 1 : desc.depth_or_array_size;
        const std::uint64_t height = desc.height;
        const std::uint32_t mips = mip_count(desc);
        std::uint64_t chain = 0;
        for (std::uint32_t mip = 0; mip < std::min(mips, 64u); ++mip) {
            const std::uint64_t elements_across = (std::max<std::uint64_t>(desc.width >> mip, 1) + block - 1) / block;
            const std::uint64_t rows = (std::max<std::uint64_t>(height >> mip, 1) + block - 1) / block;
            const std::uint64_t layers = std::max<std::uint64_t>(depth >> mip, 1);
            const auto plane = checked_mul(elements_across, rows);
            const auto volume = plane ? checked_mul(*plane, layers) : std::nullopt;
            const auto sized = volume ? checked_mul(*volume, element_bytes) : std::nullopt;
            const auto sum = sized ? checked_add(chain, *sized) : std::nullopt;
            if (!sum) return std::nullopt;
            chain = *sum;
        }
        const auto layered = checked_mul(chain, slices);
        if (!layered) return std::nullopt;
        const auto samples = checked_mul(*layered, std::max<std::uint64_t>(desc.sample_count, 1));
        if (!samples) return std::nullopt;
        bytes = *samples;
    }
    if (bytes == 0) return std::nullopt;
    const std::uint64_t minimum = desc.sample_count > 1 ? kMib4 : kKib64;
    const std::uint64_t alignment = std::max(desc.alignment, minimum);
    if (!std::has_single_bit(alignment)) return std::nullopt;
    const auto size = round_up(bytes, kKib64);
    if (!size) return std::nullopt;
    return AllocationInfo{*size, alignment};
}

std::uint32_t pc_format(std::uint32_t format) {
    switch (format) {
    case 118: return 45;
    case 119: return 46;
    case 120: return 47;
    default: return format;
    }
}

std::uint32_t family(std::uint32_t format) {
    if (format == 87) return 90;
    if (format == 88) return 92;
    for (const auto& [first, last] : kFamilies) {
        if (format >= first && format <= last) return first;
    }
    return format;
}

std::uint32_t mip_count(const ResourceDesc& desc) {
    if (desc.mip_levels != 0) return desc.mip_levels;
    const std::uint64_t depth = desc.dimension == kTexture3d ? desc.depth_or_array_size : 1;
    const std::uint64_t largest = std::max({desc.width, static_cast<std::uint64_t>(desc.height), depth,
                                            std::uint64_t{1}});
    return 64 - static_cast<std::uint32_t>(std::countl_zero(largest));
}

std::optional<std::uint64_t> footprint_bytes(const PlacedFootprint& footprint) {
    const auto format = element(footprint.format);
    if (!format) return std::nullopt;
    const std::uint64_t element_bytes = format->first;
    const std::uint64_t block = format->second;
    const std::uint64_t lines = (static_cast<std::uint64_t>(footprint.height) + block - 1) / block *
                                static_cast<std::uint64_t>(footprint.depth);
    const std::uint64_t last = ((static_cast<std::uint64_t>(footprint.width) + block - 1) / block) * element_bytes;
    const auto pitched = checked_mul(static_cast<std::uint64_t>(footprint.row_pitch), lines > 0 ? lines - 1 : 0);
    if (!pitched) return std::nullopt;
    return checked_add(*pitched, last);
}

HRESULT create_heap(Object*, const HeapDesc* desc, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    G6SHIM_TRACE("d3d12_xs: heap of %llu bytes (kind %u, flags 0x%X)",
                 static_cast<unsigned long long>(desc->size), desc->properties.kind, desc->flags);
    if (desc->alignment != 0 && !std::has_single_bit(desc->alignment)) return E_INVALIDARG;
    const AllocationInfo info{desc->size, std::max(desc->alignment, kKib64)};
    std::optional<Memory> memory;
    const HRESULT hr = Memory::create(info, &memory);
    if (FAILED(hr)) return hr;
    auto heap = std::make_unique<Heap>(*desc, std::move(*memory));
    return hand_out(kHeap, std::move(heap), iid, kHeapIids, out);
}

HRESULT create_committed(Object*, const HeapProperties* heap, std::uint32_t heap_flags, const ResourceDesc* desc,
                         std::uint32_t state, const void* clear, const Iid* iid, Object** out) {
    if (heap == nullptr || desc == nullptr) return E_INVALIDARG;
    const auto info = footprint(*desc);
    if (!info) {
        G6SHIM_WARN("d3d12_xs: no size for a resource (dimension %u, format %u)", desc->dimension, desc->format);
        return E_INVALIDARG;
    }
    G6SHIM_TRACE("d3d12_xs: committed resource of %llu bytes (dimension %u, heap %u)",
                 static_cast<unsigned long long>(info->size), desc->dimension, heap->kind);
    std::optional<Memory> memory;
    const HRESULT hr = Memory::for_resource(*info, heap->kind, &memory);
    if (FAILED(hr)) return hr;
    return hand_out_resource(*desc, *heap, heap_flags, Backing(std::move(*memory)), state, clear, iid, out);
}

HRESULT create_committed3(Object* device, const HeapProperties* heap, std::uint32_t heap_flags,
                          const ResourceDesc* desc, std::uint32_t, const void* clear, Object*, std::uint32_t,
                          const std::uint32_t*, const Iid* iid, Object** out) {
    constexpr std::uint32_t kCommon = 0;
    return create_committed(device, heap, heap_flags, desc, kCommon, clear, iid, out);
}

HRESULT register_page_pool(Object*, std::uint64_t address, std::uint32_t pages, std::size_t* out) {
    if (out == nullptr || pages == 0 || address % kKib64 != 0) return E_INVALIDARG;
    G6SHIM_TRACE("d3d12_xs: page pool at 0x%llX, %u pages", static_cast<unsigned long long>(address), pages);
    std::lock_guard<std::mutex> lock(g_pools_mutex);
    g_pools.push_back(std::pair<std::uint64_t, std::uint32_t>{address, pages});
    *out = g_pools.size();
    return S_OK;
}

HRESULT unregister_page_pool(Object*, std::size_t handle) {
    std::lock_guard<std::mutex> lock(g_pools_mutex);
    if (handle == 0 || handle > g_pools.size() || !g_pools[handle - 1]) return E_INVALIDARG;
    g_pools[handle - 1].reset();
    return S_OK;
}

HRESULT create_placed(Object*, Object* heap, std::uint64_t offset, const ResourceDesc* desc, std::uint32_t state,
                      const void* clear, const Iid* iid, Object** out) {
    if (desc == nullptr || heap == nullptr) return E_INVALIDARG;
    Heap* target = static_cast<Heap*>(heap);
    const auto info = footprint(*desc);
    const bool fits = info && offset % info->alignment == 0 && offset <= target->desc.size &&
                      info->size <= target->desc.size - offset;
    if (!fits) {
        G6SHIM_WARN("d3d12_xs: placed resource of %llu bytes at 0x%llX does not fit a heap of %llu bytes",
                    static_cast<unsigned long long>(info ? info->size : 0), static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(target->desc.size));
        return E_INVALIDARG;
    }
    add_ref(heap);
    return hand_out_resource(*desc, target->desc.properties, target->desc.flags,
                             Backing(PlacedBacking{heap, offset}), state, clear, iid, out);
}

HRESULT create_at_address(Object*, std::uint64_t address, const ResourceDesc* desc, std::uint32_t layout,
                          const void* clear, const Iid* iid, Object** out) {
    if (desc == nullptr || address == 0) return E_INVALIDARG;
    G6SHIM_TRACE("d3d12_xs: resource at 0x%llX (dimension %u, %llu wide, layout %u)",
                 static_cast<unsigned long long>(address), desc->dimension,
                 static_cast<unsigned long long>(desc->width), layout);
    constexpr std::uint32_t kCommon = 0;
    constexpr HeapProperties kDefault{1, 0, 0, 1, 1};
    return hand_out_resource(*desc, kDefault, 0, Backing(reinterpret_cast<std::uint8_t*>(address)), kCommon, clear,
                             iid, out);
}

void copyable_footprints(Object*, const ResourceDesc* desc, std::uint32_t first, std::uint32_t count,
                         std::uint64_t base, PlacedFootprint* layouts, std::uint32_t* rows,
                         std::uint64_t* row_sizes, std::uint64_t* total) {
    std::uint64_t end = base;
    bool known = desc != nullptr;
    for (std::uint32_t i = 0; i < count; ++i) {
        PlacedFootprint footprint = kNoFootprint;
        std::uint32_t row_count = std::numeric_limits<std::uint32_t>::max();
        std::uint64_t row_size = std::numeric_limits<std::uint64_t>::max();
        if (known) {
            const auto one = subresource(*desc, first + i);
            if (one) {
                const auto offset = round_up(end, kPlacement);
                const std::uint64_t lines =
                    static_cast<std::uint64_t>(one->rows) * static_cast<std::uint64_t>(one->footprint.depth);
                const auto pitched = checked_mul(static_cast<std::uint64_t>(one->footprint.row_pitch),
                                                 lines > 0 ? lines - 1 : 0);
                const auto finish = pitched ? checked_add(*pitched, one->row_size) : std::nullopt;
                const auto next = offset && finish ? checked_add(*offset, *finish) : std::nullopt;
                if (offset && next) {
                    footprint = one->footprint;
                    footprint.offset = *offset;
                    row_count = one->rows;
                    row_size = one->row_size;
                    end = *next;
                } else {
                    known = false;
                }
            } else {
                known = false;
            }
        }
        if (!known) {
            footprint = kNoFootprint;
            row_count = std::numeric_limits<std::uint32_t>::max();
            row_size = std::numeric_limits<std::uint64_t>::max();
        }
        if (layouts != nullptr) std::memcpy(&layouts[i], &footprint, sizeof footprint);
        if (rows != nullptr) rows[i] = row_count;
        if (row_sizes != nullptr) row_sizes[i] = row_size;
    }
    if (!known)
        G6SHIM_WARN("d3d12_xs: no upload footprint for this resource (dimension %u, format %u)",
                    desc ? desc->dimension : 0, desc ? desc->format : 0);
    if (total != nullptr) *total = known ? end - base : std::numeric_limits<std::uint64_t>::max();
}

AllocationInfo* allocation_info(AllocationInfo* out, Object*, std::uint32_t, std::uint32_t count,
                                const ResourceDesc* descs) {
    const std::span<const ResourceDesc> list =
        descs != nullptr ? std::span<const ResourceDesc>(descs, count) : std::span<const ResourceDesc>{};
    const AllocationInfo total = place(list, [](std::size_t, const AllocationInfo1&) {});
    if (out != nullptr) *out = total;
    return out;
}

AllocationInfo* allocation_info2(AllocationInfo* out, Object*, std::uint32_t, std::uint32_t count,
                                 const ResourceDesc1* descs, AllocationInfo1* each) {
    const AllocationInfo total = [&] {
        AllocationInfo sum{0, kKib64};
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto one = descs != nullptr ? footprint(descs[i].desc) : std::nullopt;
            if (!one) {
                G6SHIM_WARN("d3d12_xs: no size for resource %u", i);
                if (each != nullptr)
                    each[i] = AllocationInfo1{std::numeric_limits<std::uint64_t>::max(), 0,
                                              std::numeric_limits<std::uint64_t>::max()};
                return AllocationInfo{std::numeric_limits<std::uint64_t>::max(), 0};
            }
            const auto offset = round_up(sum.size, one->alignment);
            const auto end = offset ? checked_add(*offset, one->size) : std::nullopt;
            if (!offset || !end) return AllocationInfo{std::numeric_limits<std::uint64_t>::max(), 0};
            if (each != nullptr) each[i] = AllocationInfo1{*offset, one->alignment, one->size};
            sum.size = *end;
            sum.alignment = std::max(sum.alignment, one->alignment);
        }
        return sum;
    }();
    if (out != nullptr) *out = total;
    return out;
}

HeapProperties* custom_heap_properties(HeapProperties* out, Object*, std::uint32_t nodes, std::uint32_t kind) {
    const std::uint32_t node_count = std::max(nodes, 1u);
    std::uint32_t cpu_pages = 0;
    if (kind == 1)
        cpu_pages = kNotAvailable;
    else if (kind == 2 || kind == 3)
        cpu_pages = kWriteBack;
    const HeapProperties properties{kCustomHeap, cpu_pages, cpu_pages == 0 ? 0 : kPoolL0, node_count, node_count};
    if (out != nullptr) *out = properties;
    return out;
}

std::optional<std::pair<const std::uint8_t*, ResourceDesc>> texture_memory(Object* object) {
    if (object == nullptr) return std::nullopt;
    const Resource* resource = resource_of(object);
    if (resource->desc.dimension == kBuffer) return std::nullopt;
    return std::pair<const std::uint8_t*, ResourceDesc>{resource->data(), resource->desc};
}

std::optional<std::pair<std::uint8_t*, std::uint64_t>> buffer_bytes(Object* object) {
    if (object == nullptr) return std::nullopt;
    const Resource* resource = resource_of(object);
    if (resource->desc.dimension != kBuffer) return std::nullopt;
    return std::pair<std::uint8_t*, std::uint64_t>{resource->data(), resource->desc.width};
}

std::optional<std::uint32_t> view_format(Object* object) {
    const std::uint32_t own = pc_format(resource_of(object)->desc.format);
    const auto host_format = later::host_texture_format(object);
    if (host_format && *host_format != own) return own;
    return std::nullopt;
}

bool host_resource(Object* object) { return object != nullptr && resource_of(object)->host != nullptr; }

std::shared_ptr<later::HostTexture> host_copy(Object* object) {
    if (object == nullptr) return nullptr;
    return resource_of(object)->host;
}

std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* object, std::uint32_t wanted) {
    return later::host_transition(object, wanted);
}

}  // namespace g6shim::d3d12_xs::resource
