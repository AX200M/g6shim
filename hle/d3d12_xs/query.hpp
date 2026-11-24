// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <cstdint>

namespace g6shim::d3d12_xs::query {

inline constexpr std::array<Iid, 4> kIids{
    guid(0x0d9658ae, 0xed45, 0x469e, {0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

struct Desc {
    std::uint32_t kind;
    std::uint32_t count;
    std::uint32_t nodes;
};
static_assert(sizeof(Desc) == 12);

HRESULT create(Object* device, const Desc* desc, const Iid* iid, Object** out);

}  // namespace g6shim::d3d12_xs::query
