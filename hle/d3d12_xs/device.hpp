// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace g6shim::d3d12_xs::device {

inline constexpr Iid kIid =
    guid(0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7});

inline constexpr std::array<Iid, 9> kLevelIids{
    guid(0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}),
    guid(0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}),
    guid(0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}),
    guid(0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}),
    guid(0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}),
    guid(0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc}),
    guid(0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}),
    guid(0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}),
    guid(0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}),
};

inline constexpr std::uint32_t kCheckedVersion = 0x0002'0004;
inline constexpr std::uint32_t kHighestFeatureLevel = 0xC200;

const std::array<const void*, kMaxSlots>& table();

struct Device : Object {
    Device() : Object(table().data()) {}

    std::uint64_t null_constant_buffer = 0;
    std::array<std::uint32_t, 16> out_of_line{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(Device) == 0x58);

Object* device();
bool newer();
bool is_device(const Iid* iid);

HRESULT create(const void* adapter, const std::uint32_t* parameters, const Iid* iid, Object** out);

struct VirtualMemory {
    std::uint32_t flags;
    std::uint32_t page_table_pages;
    std::uint32_t preserved_bytes;
    std::uint32_t reserved;
    std::uint64_t lowest;
    std::uint64_t highest;
};
static_assert(sizeof(VirtualMemory) == 0x20);

HRESULT configure_virtual_memory(VirtualMemory* config);

struct FeatureLevels {
    std::uint32_t count;
    const std::uint32_t* requested;
    std::uint32_t highest;
};
static_assert(sizeof(FeatureLevels) == 0x18);

}  // namespace g6shim::d3d12_xs::device
