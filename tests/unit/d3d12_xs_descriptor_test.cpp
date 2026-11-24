// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/object.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

struct Extended {
    descriptor::HeapDesc desc;
    descriptor::Placement placement;
};

HRESULT create_heap_with(const descriptor::HeapDesc& desc, const descriptor::Placement& placement, Object** out) {
    const Extended extended{desc, placement};
    return descriptor::create_heap(nullptr, &extended.desc, &descriptor::kIids[0], out);
}

}  // namespace

TEST(d3d12_xs_heaps_hold_count_descriptors_from_their_start) {
    const descriptor::HeapDesc desc{0, 1000, 1, 0};
    const descriptor::Placement none{0, 0};
    Object* heap = nullptr;
    CHECK(create_heap_with(desc, none, &heap) == S_OK);
    const auto cpu = slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap);
    CHECK(cpu % 64 == 0);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x50)(heap) == cpu);
    CHECK(descriptor::increment_size(nullptr, 0) == 32);
    *reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(cpu) + 999 * 32 + 31) = 1;
    CHECK(descriptor::increment_size(nullptr, 9) == 0);
    const descriptor::HeapDesc bad{9, 1000, 1, 0};
    CHECK(create_heap_with(bad, none, &heap) == E_INVALIDARG);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
}

TEST(d3d12_xs_views_survive_copies) {
    std::array<std::uint64_t, 16> heap{};
    const auto at = [&heap](std::size_t index) {
        return reinterpret_cast<std::uint64_t>(heap.data()) + 32 * index;
    };
    const std::uint64_t s0 = at(0), s1 = at(1), s2 = at(2), s3 = at(3);
    const std::array<std::uint32_t, 4> constant{0x1000, 1, 256, 0};
    descriptor::create_cbv(nullptr, constant.data(), s0);
    CHECK(descriptor::view(s0) == std::optional<descriptor::View>(descriptor::ConstantView{0x1'0000'1000, 256}));
    descriptor::create_srv(nullptr, nullptr, nullptr, s1);
    CHECK(descriptor::view(s1) == std::optional<descriptor::View>(descriptor::ShaderView{0, std::nullopt}));
    descriptor::create_srv(nullptr, nullptr, nullptr, s2);
    std::array<std::uint32_t, 3> first_words{};
    std::array<std::uint32_t, 3> second_words{};
    std::memcpy(first_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(s1)),
                sizeof first_words);
    std::memcpy(second_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(s2)),
                sizeof second_words);
    CHECK(first_words == second_words);

    const std::array<std::uint64_t, 1> destination{s2};
    const std::array<std::uint32_t, 1> destination_sizes{2};
    const std::array<std::uint64_t, 2> source{s0, s1};
    const std::array<std::uint32_t, 2> source_sizes{1, 1};
    descriptor::copy(nullptr, 1, destination.data(), destination_sizes.data(), 2, source.data(),
                     source_sizes.data(), 0);
    CHECK(descriptor::view(s2) == descriptor::view(s0));
    CHECK(descriptor::view(s3) == descriptor::view(s1));
    CHECK(descriptor::view(at(0) + 8) == std::nullopt);
}

TEST(d3d12_xs_views_die_with_their_resource) {
    std::array<std::uint64_t, 8> slots{};
    const auto a = reinterpret_cast<std::uint64_t>(slots.data());
    const auto b = a + 32;
    auto* resource = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x7777'0000));
    descriptor::create_srv(nullptr, resource, nullptr, a);
    CHECK(descriptor::view(a) ==
          std::optional<descriptor::View>(descriptor::ShaderView{0x7777'0000, std::nullopt}));
    descriptor::forget(resource);
    CHECK(descriptor::view(a) == std::nullopt);
    descriptor::create_srv(nullptr, resource, nullptr, b);
    CHECK(descriptor::view(b) ==
          std::optional<descriptor::View>(descriptor::ShaderView{0x7777'0000, std::nullopt}));
    std::array<std::uint32_t, 3> old_words{};
    std::array<std::uint32_t, 3> new_words{};
    std::memcpy(old_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof old_words);
    std::memcpy(new_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(b)), sizeof new_words);
    CHECK(old_words != new_words);
}

TEST(d3d12_xs_placed_heaps_start_at_the_title_memory) {
    std::vector<std::uint8_t> memory(64 * 32);
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    const descriptor::HeapDesc desc{0, 64, 1, 1};
    Object* heap = nullptr;
    CHECK(create_heap_with(desc, descriptor::Placement{address, 64 * 32}, &heap) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap) == address);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x50)(heap) == address);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
    CHECK(create_heap_with(desc, descriptor::Placement{address, 7}, &heap) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap) != address);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
}
