// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/device.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/resource.hpp"

#include <cstdint>
#include <limits>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

constexpr resource::HeapProperties kProperties{1, 0, 0, 0, 0};

resource::ResourceDesc buffer(std::uint64_t width) {
    resource::ResourceDesc desc{};
    desc.dimension = resource::kBuffer;
    desc.width = width;
    desc.height = 1;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 1;
    desc.sample_count = 1;
    desc.layout = 1;
    return desc;
}

resource::ResourceDesc texture(std::uint64_t width, std::uint32_t height, std::uint32_t format,
                               std::uint16_t mips) {
    resource::ResourceDesc desc = buffer(0);
    desc.dimension = 3;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.mip_levels = mips;
    desc.layout = 0;
    return desc;
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

Object* make_heap(std::uint64_t size) {
    const resource::HeapDesc desc{size, kProperties, 0, 0};
    Object* heap = nullptr;
    CHECK(resource::create_heap(nullptr, &desc, &resource::kHeapIids[0], &heap) == S_OK);
    return heap;
}

}  // namespace

TEST(d3d12_xs_heaps_are_zeroed_aligned_memory) {
    Object* heap = make_heap(1 << 20);
    auto* memory_heap = static_cast<resource::Heap*>(heap);
    CHECK(memory_heap->memory.address() % resource::kKib64 == 0);
    resource::HeapDesc back{};
    slot<void (*)(Object*, resource::HeapDesc*)>(heap, 0x40)(heap, &back);
    CHECK(back.size == 1 << 20);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);

    const resource::HeapDesc bad{1 << 20, kProperties, 3, 0};
    Object* out = nullptr;
    CHECK(resource::create_heap(nullptr, &bad, &resource::kHeapIids[0], &out) == E_INVALIDARG);
}

TEST(d3d12_xs_device_setup_heap_properties_and_resources_at_addresses) {
    Object* device = device::device();
    using Custom = resource::HeapProperties* (*)(resource::HeapProperties*, Object*, std::uint32_t, std::uint32_t);
    resource::HeapProperties properties = kProperties;
    const auto custom = slot<Custom>(device, 0xd0);
    CHECK(custom(&properties, device, 1, 2) == &properties);
    CHECK(properties.kind == 4);
    CHECK(properties.cpu_pages == 3);
    CHECK(properties.pool == 1);
    CHECK(properties.creation_nodes == 1);
    CHECK(properties.visible_nodes == 1);
    custom(&properties, device, 1, 1);
    CHECK(properties.cpu_pages == 1);

    using At = HRESULT (*)(Object*, std::uint64_t, const resource::ResourceDesc*, std::uint32_t, const void*,
                           const Iid*, Object**);
    std::vector<std::uint8_t> memory(0x1'0000);
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    Object* resource = nullptr;
    const resource::ResourceDesc desc = buffer(0x1'0000);
    const auto create = slot<At>(device, 0x428);
    CHECK(create(device, address, &desc, 0xFFFF'FFFF, nullptr, &resource::kResourceIids[0], &resource) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(resource, 0x58)(resource) == address);
    CHECK(slot<std::uint32_t (*)(Object*)>(resource, 0x10)(resource) == 0);
    CHECK(memory[0xFFFF] == 0);
    CHECK(create(device, 0, &desc, 0xFFFF'FFFF, nullptr, &resource::kResourceIids[0], &resource) != S_OK);
}

TEST(d3d12_xs_sizes_cover_every_mip) {
    const auto size = [](const resource::ResourceDesc& desc) {
        const auto info = resource::footprint(desc);
        return info ? std::optional<std::uint64_t>(info->size) : std::nullopt;
    };
    CHECK(size(buffer(256)) == resource::kKib64);
    const resource::ResourceDesc rgba = texture(256, 256, 28, 0);
    const auto rgba_layout = resource::texture_layout(rgba);
    CHECK(rgba_layout.has_value());
    if (rgba_layout)
        CHECK(size(rgba) == round_up(rgba_layout->size, 4096));
    const auto bc7 = resource::footprint(texture(512, 512, 98, 10));
    CHECK(bc7.has_value());
    if (bc7) {
        CHECK(bc7->size == 0x56000);
        CHECK(bc7->alignment == 0x1000);
    }
    if (rgba_layout) CHECK(rgba_layout->size >= 4ull * (256 * 256 * 4) / 3);
    CHECK(size(texture(1024, 1024, 71, 1)) == 512ull << 10);
    CHECK(!size(texture(16, 16, 66, 1)).has_value());

    resource::ResourceDesc linear = texture(100, 60, 28, 1);
    linear.layout = 1;
    const auto linear_layout = resource::texture_layout(linear);
    CHECK(linear_layout.has_value());
    if (linear_layout) CHECK(linear_layout->planes[0].layout.desc.swizzle == 0);
    resource::ResourceDesc display = texture(100, 60, 28, 1);
    display.layout = 0x10A;
    const auto display_layout = resource::texture_layout(display);
    CHECK(display_layout.has_value());
    if (display_layout) CHECK(display_layout->planes[0].layout.desc.swizzle == 10);

    resource::AllocationInfo info{};
    const std::array<resource::ResourceDesc, 2> descs{buffer(100), texture(256, 256, 28, 0)};
    resource::allocation_info(&info, nullptr, 0, 2, descs.data());
    CHECK(info.size == resource::kKib64 + size(descs[1]).value_or(0));
    CHECK(info.alignment == resource::kKib64);
    const resource::ResourceDesc unknown = texture(1, 1, 999, 1);
    resource::allocation_info(&info, nullptr, 0, 1, &unknown);
    CHECK(info.size == std::numeric_limits<std::uint64_t>::max());
}

TEST(d3d12_xs_upload_footprints_follow_the_pc_rules) {
    std::array<resource::PlacedFootprint, 2> layouts{};
    std::array<std::uint32_t, 2> rows{};
    std::array<std::uint64_t, 2> sizes{};
    std::uint64_t total = 0;
    const auto footprints = [&](const resource::ResourceDesc& desc, std::uint32_t first, std::uint32_t count) {
        resource::copyable_footprints(nullptr, &desc, first, count, 0, layouts.data(), rows.data(), sizes.data(),
                                      &total);
    };
    const resource::ResourceDesc rgba = texture(100, 60, 28, 2);
    footprints(rgba, 0, 2);
    CHECK(layouts[0].row_pitch == 512);
    CHECK(rows[0] == 60);
    CHECK(sizes[0] == 400);
    CHECK(layouts[1].offset == round_up(512ull * 59 + 400, 512));
    CHECK(layouts[1].width == 50);
    CHECK(layouts[1].height == 30);
    CHECK(rows[1] == 30);
    CHECK(total == layouts[1].offset + 256ull * 29 + 200);
    const resource::ResourceDesc bc1 = texture(64, 64, 71, 1);
    footprints(bc1, 0, 1);
    CHECK(rows[0] == 16);
    CHECK(sizes[0] == 128);
    footprints(bc1, 1, 1);
    CHECK(total == std::numeric_limits<std::uint64_t>::max());
}

TEST(d3d12_xs_committed_and_placed_resources_map_at_their_gpu_address) {
    Object* committed = nullptr;
    const resource::ResourceDesc small = buffer(256);
    CHECK(resource::create_committed(nullptr, &kProperties, 0, &small, 0, nullptr, &resource::kResourceIids[0],
                                     &committed) == S_OK);
    std::uint8_t* data = nullptr;
    CHECK(slot<HRESULT (*)(Object*, std::uint32_t, const void*, std::uint8_t**)>(committed, 0x40)(
              committed, 0, nullptr, &data) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(committed, 0x58)(committed) ==
          reinterpret_cast<std::uint64_t>(data));
    CHECK(data[255] == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(committed, 0x10)(committed) == 0);

    Object* heap = make_heap(4 * resource::kKib64);
    const auto place = [&](std::uint64_t offset, const resource::ResourceDesc& desc, Object** out) {
        return resource::create_placed(nullptr, heap, offset, &desc, 0, nullptr, &resource::kResourceIids[0], out);
    };
    Object* placed = nullptr;
    const resource::ResourceDesc page = buffer(resource::kKib64);
    CHECK(place(resource::kKib64, page, &placed) == S_OK);
    const auto base = static_cast<resource::Heap*>(heap)->memory.address();
    CHECK(slot<std::uint64_t (*)(Object*)>(placed, 0x58)(placed) == base + resource::kKib64);
    Object* other = nullptr;
    const resource::ResourceDesc one = buffer(1);
    CHECK(place(4 * resource::kKib64, one, &other) == E_INVALIDARG);
    CHECK(place(100, one, &other) == E_INVALIDARG);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 1);
    CHECK(slot<std::uint32_t (*)(Object*)>(placed, 0x10)(placed) == 0);
}
