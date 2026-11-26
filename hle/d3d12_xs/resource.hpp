// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx10_layout.hpp"
#include "later.hpp"
#include "object.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace g6shim::d3d12_xs::resource {

inline constexpr std::array<Iid, 4> kHeapIids{
    guid(0x6b3b2502, 0x6e51, 0x45b3, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};
inline constexpr std::array<Iid, 4> kResourceIids{
    guid(0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

inline constexpr std::uint64_t kKib64 = 64 << 10;
inline constexpr std::uint64_t kMib4 = 4 << 20;

inline constexpr std::uint32_t kBuffer = 1;
inline constexpr std::uint32_t kTexture3d = 4;

inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kDepthFormats{{
    {20, 19},
    {40, 39},
    {45, 44},
    {55, 53},
}};

inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 3> kSrgbFormats{{
    {29, 28},
    {91, 87},
    {93, 88},
}};

struct HeapProperties {
    std::uint32_t kind;
    std::uint32_t cpu_pages;
    std::uint32_t pool;
    std::uint32_t creation_nodes;
    std::uint32_t visible_nodes;
};
static_assert(sizeof(HeapProperties) == 20);

struct HeapDesc {
    std::uint64_t size;
    HeapProperties properties;
    std::uint64_t alignment;
    std::uint32_t flags;
    std::uint32_t reserved;
};
static_assert(sizeof(HeapDesc) == 48);

struct ResourceDesc {
    std::uint32_t dimension;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array_size;
    std::uint16_t mip_levels;
    std::uint32_t format;
    std::uint32_t sample_count;
    std::uint32_t sample_quality;
    std::uint32_t layout;
    std::uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 56);

struct AllocationInfo {
    std::uint64_t size;
    std::uint64_t alignment;
};
static_assert(sizeof(AllocationInfo) == 16);

struct PlacedFootprint {
    std::uint64_t offset;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t row_pitch;
};
static_assert(sizeof(PlacedFootprint) == 32);

struct ResourceDesc1 {
    ResourceDesc desc;
    std::uint32_t mip_region[3];
};
static_assert(sizeof(ResourceDesc1) == 72);

struct AllocationInfo1 {
    std::uint64_t offset;
    std::uint64_t alignment;
    std::uint64_t size;
};
static_assert(sizeof(AllocationInfo1) == 24);

using AllocRoutine = void*(WINAPI*)(SIZE_T bytes, ULONGLONG attributes);
using FreeRoutine = void(WINAPI*)(void* block, ULONGLONG attributes);
using PageMapper = bool (*)(std::uint64_t to, std::uint64_t from, std::uint64_t bytes);

void use_allocator(AllocRoutine allocate, FreeRoutine free);
void use_page_mapper(PageMapper mapper);
std::optional<PageMapper> page_mapper();

class Memory {
public:
    Memory(Memory&& other) noexcept;
    Memory& operator=(Memory&& other) noexcept;
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    static HRESULT create(AllocationInfo info, std::optional<Memory>* out);
    static HRESULT zeroed(std::uint64_t size, std::uint64_t alignment, std::optional<Memory>* out);
    static HRESULT for_resource(AllocationInfo info, std::uint32_t heap_kind, std::optional<Memory>* out);

    std::uint8_t* data() const noexcept { return pointer_; }
    std::uint64_t address() const noexcept { return reinterpret_cast<std::uint64_t>(pointer_); }

private:
    Memory(std::uint8_t* pointer, std::uint64_t size, std::uint64_t alignment,
           std::optional<std::uint64_t> attributes) noexcept;
    void reset() noexcept;

    std::uint8_t* pointer_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t alignment_ = 0;
    std::optional<std::uint64_t> attributes_;
};

const std::array<const void*, 24>& table();

struct Heap : Object {
    Heap(const HeapDesc& description, Memory memory)
        : Object(table().data()), desc(description), memory(std::move(memory)) {}

    HeapDesc desc;
    Memory memory;
};

std::optional<std::pair<std::uint64_t, std::uint64_t>> element(std::uint32_t format);
std::optional<gfx10::Texture> texture_layout(const ResourceDesc& desc);
std::optional<AllocationInfo> footprint(const ResourceDesc& desc);
std::uint32_t pc_format(std::uint32_t format);
std::uint32_t family(std::uint32_t format);
std::uint32_t mip_count(const ResourceDesc& desc);
std::optional<std::uint64_t> footprint_bytes(const PlacedFootprint& footprint);

HRESULT create_heap(Object* device, const HeapDesc* desc, const Iid* iid, Object** out);
HRESULT create_committed(Object* device, const HeapProperties* heap, std::uint32_t heap_flags,
                         const ResourceDesc* desc, std::uint32_t state, const void* clear, const Iid* iid,
                         Object** out);
HRESULT create_committed3(Object* device, const HeapProperties* heap, std::uint32_t heap_flags,
                          const ResourceDesc* desc, std::uint32_t layout, const void* clear, Object* protected_session,
                          std::uint32_t castable_count, const std::uint32_t* castable, const Iid* iid, Object** out);
HRESULT create_placed(Object* device, Object* heap, std::uint64_t offset, const ResourceDesc* desc,
                      std::uint32_t state, const void* clear, const Iid* iid, Object** out);
HRESULT create_at_address(Object* device, std::uint64_t address, const ResourceDesc* desc, std::uint32_t layout,
                          const void* clear, const Iid* iid, Object** out);
HRESULT register_page_pool(Object* device, std::uint64_t address, std::uint32_t pages, std::size_t* out);
HRESULT unregister_page_pool(Object* device, std::size_t handle);

void copyable_footprints(Object* device, const ResourceDesc* desc, std::uint32_t first, std::uint32_t count,
                         std::uint64_t base, PlacedFootprint* layouts, std::uint32_t* rows,
                         std::uint64_t* row_sizes, std::uint64_t* total);
AllocationInfo* allocation_info(AllocationInfo* out, Object* device, std::uint32_t visible, std::uint32_t count,
                                const ResourceDesc* descs);
AllocationInfo* allocation_info2(AllocationInfo* out, Object* device, std::uint32_t visible, std::uint32_t count,
                                 const ResourceDesc1* descs, AllocationInfo1* each);
HeapProperties* custom_heap_properties(HeapProperties* out, Object* device, std::uint32_t nodes, std::uint32_t kind);

std::optional<std::pair<const std::uint8_t*, ResourceDesc>> texture_memory(Object* resource_object);
std::optional<std::pair<std::uint8_t*, std::uint64_t>> buffer_bytes(Object* resource_object);
std::optional<std::uint32_t> view_format(Object* resource_object);
bool host_resource(Object* resource_object);
HRESULT create_at_address_from_mip(std::uint64_t address, const ResourceDesc* desc, std::uint32_t base_mip,
                                   Object** out);
std::uint32_t host_base_mip(Object* resource_object);
std::uint32_t release_resource(Object* resource_object);

struct TextureObjects {
    std::int64_t committed = 0;
    std::int64_t placed = 0;
    std::int64_t at_address = 0;
};

TextureObjects texture_objects();
std::shared_ptr<later::HostTexture> host_copy(Object* resource_object);
std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* resource_object, std::uint32_t wanted);

}  // namespace g6shim::d3d12_xs::resource
