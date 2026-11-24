// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx10_layout.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g6shim::xg_xs {

inline constexpr std::uint32_t kSwizzledLayoutFlag = 0x100;
inline constexpr std::uint32_t kRowMajorLayout = 1;
inline constexpr std::uint32_t kAllowRenderTarget = 1;
inline constexpr std::uint32_t kAllowDepthStencil = 2;
inline constexpr std::uint32_t kTexture1D = 2;
inline constexpr std::uint32_t kTexture2D = 3;
inline constexpr std::uint32_t kTexture3D = 4;

inline constexpr std::size_t kMaxComputerMips = 15;
inline constexpr std::size_t kMaxComputerPlanes = 4;
inline constexpr std::uint32_t kPlaneUsageColor = 1;
inline constexpr std::uint32_t kPlaneUsageDepth = 7;
inline constexpr std::uint32_t kPlaneUsageStencil = 8;
inline constexpr std::uint32_t kTailAlignment = 256;
inline constexpr std::uint32_t kTileBytes = 64 * 1024;

struct ResourceDesc {
    std::uint32_t dimension;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array;
    std::uint16_t mips;
    std::uint32_t format;
    std::uint32_t samples;
    std::uint32_t quality;
    std::uint32_t layout;
    std::uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38);
static_assert(offsetof(ResourceDesc, width) == 0x10);
static_assert(offsetof(ResourceDesc, layout) == 0x2C);

struct MipOut {
    std::uint64_t size;
    std::uint64_t offset;
    std::uint64_t slice_size;
    std::uint32_t pitch_texels;
    std::uint32_t pitch_bytes;
    std::uint32_t alignment;
    std::uint32_t padded[3];
    std::uint32_t elements[2];
    std::uint32_t texels[2];
    std::uint32_t depth_or_array;
    std::uint32_t samples;
    std::uint32_t swizzle;
    std::uint64_t bank_rotation[2];
    std::uint32_t slice_depth;
    std::uint32_t in_tail;
    std::uint32_t tail_position;
};
static_assert(sizeof(MipOut) == 0x70);
static_assert(offsetof(MipOut, swizzle) == 0x48);
static_assert(offsetof(MipOut, bank_rotation) == 0x50);
static_assert(offsetof(MipOut, tail_position) == 0x68);

struct PlaneOut {
    std::uint32_t usage;
    std::uint64_t size;
    std::uint64_t slice_size;
    std::uint64_t offset;
    std::uint64_t alignment;
    std::uint32_t element_bytes;
    std::uint32_t standard_mips;
    std::uint32_t tail_mips;
    MipOut mips[kMaxComputerMips];
};
static_assert(offsetof(PlaneOut, mips) == 0x38);
static_assert(sizeof(PlaneOut) == 0x6C8);

struct LayoutOut {
    std::uint64_t size;
    std::uint64_t alignment;
    std::uint32_t mips;
    std::uint32_t planes;
    PlaneOut plane[kMaxComputerPlanes];
    std::uint32_t dimension;
};
static_assert(offsetof(LayoutOut, plane) == 0x18);
static_assert(offsetof(LayoutOut, dimension) == 0x1B38);
static_assert(sizeof(LayoutOut) == 0x1B40);

struct PackedMips {
    std::uint8_t standard;
    std::uint8_t packed;
    std::uint32_t tiles;
    std::uint32_t start;
};

struct SubresourceTiling {
    std::uint32_t width;
    std::uint16_t height;
    std::uint16_t depth;
    std::uint32_t start;
};

struct Computer {
    const void* const* vtable;
    std::atomic<std::uint32_t> refs;
    ResourceDesc desc;
    std::uint32_t mips;
    gfx10::Texture texture;
};

HRESULT create_texture_computer(const ResourceDesc* desc, Computer** out);
HRESULT suggest_swizzle_mode(const ResourceDesc* desc, std::uint32_t flags, std::uint32_t* out);

}  // namespace g6shim::xg_xs
