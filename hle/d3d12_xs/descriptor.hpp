// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"
#include "resource.hpp"

#include <d3d12.h>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace g6shim::d3d12_xs::descriptor {

inline constexpr std::array<Iid, 4> kIids{
    guid(0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

inline constexpr std::uint32_t kViewsKind = 0;
inline constexpr std::uint32_t kSamplersKind = 1;
inline constexpr std::uint32_t kShaderVisible = 1;

struct HeapDesc {
    std::uint32_t kind;
    std::uint32_t count;
    std::uint32_t flags;
    std::uint32_t nodes;
};
static_assert(sizeof(HeapDesc) == 16);

struct Placement {
    std::uint64_t address;
    std::uint64_t size;
};
static_assert(sizeof(Placement) == 16);

struct Heap : Object {
    Heap(const HeapDesc& description, std::uint64_t first, std::optional<resource::Memory> storage);
    ~Heap();

    bool samplers() const { return desc.kind == kSamplersKind; }
    std::optional<std::uint32_t> index(std::uint64_t handle) const;

    HeapDesc desc;
    std::uint64_t start;
    std::optional<resource::Memory> memory;
};

std::optional<std::uint32_t> increment(std::uint32_t kind);
std::uint32_t increment_size(Object* device, std::uint32_t kind);

struct RtvRecord {
    std::uint32_t tag;
    std::uint32_t described;
    Object* resource;
    std::array<std::uint32_t, 6> desc;
};
static_assert(sizeof(RtvRecord) <= 112);

struct ConstantView {
    std::uint64_t address;
    std::uint32_t size;
    auto operator<=>(const ConstantView&) const = default;
};

struct ShaderView {
    std::size_t resource;
    std::optional<std::array<std::uint32_t, 10>> desc;
    auto operator<=>(const ShaderView&) const = default;
};

struct UnorderedView {
    std::size_t resource;
    std::size_t counter;
    std::optional<std::array<std::uint32_t, 10>> desc;
    auto operator<=>(const UnorderedView&) const = default;
};

struct SamplerView {
    std::array<std::uint32_t, 13> desc;
    auto operator<=>(const SamplerView&) const = default;
};

struct ConsoleSamplerView {
    std::array<std::uint32_t, 27> desc;
    auto operator<=>(const ConsoleSamplerView&) const = default;
};

struct PlacedView {
    bool unordered;
    std::array<std::uint32_t, 14> resource;
    std::array<std::uint32_t, 16> desc;
    auto operator<=>(const PlacedView&) const = default;
};

using View = std::variant<ConstantView, ShaderView, UnorderedView, SamplerView, ConsoleSamplerView, PlacedView>;

HRESULT create_heap(Object* device, const HeapDesc* desc, const Iid* iid, Object** out);
void create_cbv(Object* device, const std::uint32_t* desc, std::uint64_t handle);
void create_srv(Object* device, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle);
void create_uav(Object* device, Object* resource_object, Object* counter, const std::uint32_t* desc,
                std::uint64_t handle);
void create_rtv(Object* device, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle);
void create_dsv(Object* device, Object* resource_object, const std::uint32_t* desc, std::uint64_t handle);
void create_sampler(Object* device, const std::uint32_t* desc, std::uint64_t handle);
void create_sampler_x(Object* device, const std::uint32_t* desc, std::uint64_t handle);
void create_placed_srv(Object* device, const std::uint32_t* resource_desc, const std::uint32_t* desc,
                       std::uint64_t handle);
void create_placed_uav(Object* device, const std::uint32_t* resource_desc, const std::uint32_t* desc,
                       std::uint64_t handle);
void copy_simple(Object* device, std::uint32_t count, std::uint64_t destination, std::uint64_t source,
                 std::uint32_t kind);
void copy(Object* device, std::uint32_t destination_ranges, const std::uint64_t* destination_starts,
          const std::uint32_t* destination_sizes, std::uint32_t source_ranges, const std::uint64_t* source_starts,
          const std::uint32_t* source_sizes, std::uint32_t kind);

void forget(Object* resource_object);
std::optional<View> view(std::uint64_t handle);
std::optional<RtvRecord> rtv_record(std::uint64_t handle);
std::optional<RtvRecord> dsv_record(std::uint64_t handle);
std::optional<D3D12_RENDER_TARGET_VIEW_DESC> rtv_description(const RtvRecord& record);
std::optional<D3D12_DEPTH_STENCIL_VIEW_DESC> dsv_description(const RtvRecord& record);
void release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count);

}  // namespace g6shim::d3d12_xs::descriptor
