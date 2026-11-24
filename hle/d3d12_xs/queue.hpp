// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <cstdint>

namespace g6shim::d3d12_xs::queue {

inline constexpr std::array<Iid, 4> kIids{
    guid(0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

struct Desc {
    std::uint32_t kind;
    std::int32_t priority;
    std::uint32_t flags;
    std::uint32_t nodes;
};
static_assert(sizeof(Desc) == 16);

struct DescX {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t engine_or_pipe;
    std::uint32_t queue;
};
static_assert(sizeof(DescX) == 16);

struct Plane {
    std::uint64_t token;
    std::uint32_t count;
    Object* const* resources;
};
static_assert(sizeof(Plane) == 24);

struct PageRange {
    std::uint32_t kind;
    std::uint32_t start;
    std::uint32_t count;
};
static_assert(sizeof(PageRange) == 12);

struct PageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const PageRange* ranges;
};
static_assert(sizeof(PageBatch) == 24);

HRESULT create(Object* device, const Desc* desc, const Iid* iid, Object** out);
HRESULT create_x(Object* device, const DescX* desc, const Iid* iid, Object** out);

const std::array<const void*, 36>& functions(bool newer);

}  // namespace g6shim::d3d12_xs::queue
