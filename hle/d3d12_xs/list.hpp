// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "draw.hpp"
#include "host.hpp"
#include "object.hpp"
#include "resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace g6shim::d3d12_xs::list {

inline constexpr std::array<Iid, 4> kAllocatorIids{
    guid(0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

inline constexpr std::array<Iid, 10> kListIids{
    guid(0x7116d91c, 0xe7e4, 0x47ce, {0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5}),
    guid(0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}),
    guid(0x553103fb, 0x1fe7, 0x4557, {0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7}),
    guid(0x38c3e585, 0xff17, 0x412c, {0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28}),
    guid(0x6fda83a7, 0xb84c, 0x4e38, {0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d}),
    guid(0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}),
    guid(0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}),
    guid(0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}),
    kPageableIids[1],
    kPageableIids[2],
};

struct ListDesc {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t nodes;
    std::uint32_t barrier_batch;
};
static_assert(sizeof(ListDesc) == 16);

struct Barrier {
    std::uint32_t kind;
    std::uint32_t flags;
    const void* first;
    std::uint64_t second;
    std::uint64_t last;
};
static_assert(sizeof(Barrier) == 32);

struct CopyLocation {
    Object* resource;
    std::uint32_t kind;
    resource::PlacedFootprint footprint;
};
static_assert(sizeof(CopyLocation) == 48);

HRESULT create_allocator(Object* device, std::uint32_t kind, const Iid* iid, Object** out);
HRESULT create_list(Object* device, std::uint32_t nodes, std::uint32_t kind, Object* allocator, Object* pipeline,
                    const Iid* iid, Object** out);
HRESULT create_list_x(Object* device, const ListDesc* desc, Object* allocator, Object* pipeline, const Iid* iid,
                      Object** out);

void* allocate_upload(Object* title_list, std::uint64_t size, std::uint64_t alignment);
std::size_t upload_count(Object* title_list);
host::HostList* host_list(Object* title_list);
std::pair<std::uint64_t, std::uint64_t> take_counts();
const std::array<const void*, kMaxSlots>& functions(bool newer);

}  // namespace g6shim::d3d12_xs::list
