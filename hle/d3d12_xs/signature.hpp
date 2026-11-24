// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace g6shim::d3d12_xs::signature {

inline constexpr std::array<Iid, 4> kIids{
    guid(0xc36a797c, 0xec80, 0x4f0a, {0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

struct Desc {
    std::uint32_t stride;
    std::uint32_t count;
    const std::array<std::uint32_t, 4>* arguments;
    std::uint32_t nodes;
};
static_assert(sizeof(Desc) == 24);

struct Layout {
    std::uint32_t stride;
    const std::vector<std::array<std::uint32_t, 4>>* arguments;
};

HRESULT create(Object* device, const Desc* desc, Object* root, const Iid* iid, Object** out);
std::optional<Layout> layout(Object* signature);

}  // namespace g6shim::d3d12_xs::signature
