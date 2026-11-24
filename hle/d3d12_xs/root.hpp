// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace g6shim::d3d12_xs::root {

inline constexpr std::array<Iid, 3> kIids{
    guid(0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}),
    kPageableIids[1],
    kPageableIids[2],
};

struct Desc {
    std::uint32_t parameter_count;
    const void* parameters;
    std::uint32_t sampler_count;
    const void* samplers;
    std::uint32_t flags;
};
static_assert(sizeof(Desc) == 0x28);

struct VersionedDesc {
    std::uint32_t version;
    Desc desc;
};
static_assert(sizeof(VersionedDesc) == 0x30);

HRESULT serialize(const Desc* desc, std::uint32_t version, void** blob, void** error);
HRESULT serialize_versioned(const VersionedDesc* desc, void** blob, void** error);
HRESULT create_deserializer(const std::uint8_t* blob, std::size_t size, const Iid* iid, void** out);
HRESULT create(Object* device, std::uint32_t nodes, const std::uint8_t* blob, std::size_t size, const Iid* iid,
               Object** out);

}  // namespace g6shim::d3d12_xs::root
