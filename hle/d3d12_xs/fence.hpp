// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <cstdint>

namespace g6shim::d3d12_xs::fence {

inline constexpr std::array<Iid, 4> kIids{
    guid(0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

HRESULT create(Object* device, std::uint64_t initial, std::uint32_t flags, const Iid* iid, Object** out);
HRESULT signal(Object* fence, std::uint64_t value);

}  // namespace g6shim::d3d12_xs::fence
