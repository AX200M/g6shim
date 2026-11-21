// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::xg {

inline constexpr std::uint32_t kMaxMipLevels = 15;
inline constexpr std::uint32_t kMaxPlanes = 4;
inline constexpr std::uint32_t kResourceDimensionTexture2D = 3;
inline constexpr std::uint32_t kPlaneUsageDefault = 1;
inline constexpr std::uint32_t kTileModeLinearGeneral = 8;
inline constexpr std::uint32_t kTileModeThin1D = 13;
inline constexpr std::uint32_t kMicroTileElements = 8;
inline constexpr std::uint32_t kTiledSliceAlignment = 256;
inline constexpr std::uint32_t kRowPitchAlignment = 256;
inline constexpr std::uint32_t kPlacementAlignment = 512;

struct Texture2DDesc {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mip_levels;
    std::uint32_t array_size;
    std::uint32_t format;
    std::uint32_t sample_count;
    std::uint32_t sample_quality;
    std::uint32_t usage;
    std::uint32_t bind_flags;
    std::uint32_t cpu_access_flags;
    std::uint32_t misc_flags;
    std::uint32_t esram_offset_bytes;
    std::uint32_t esram_usage_bytes;
    std::uint32_t tile_mode;
    std::uint32_t pitch;
};
static_assert(sizeof(Texture2DDesc) == 0x3C);
static_assert(offsetof(Texture2DDesc, tile_mode) == 0x34);

struct MipLayout {
    std::uint64_t size_bytes;
    std::uint64_t offset_bytes;
    std::uint64_t slice_2d_size_bytes;
    std::uint32_t pitch_pixels;
    std::uint32_t pitch_bytes;
    std::uint32_t alignment_bytes;
    std::uint32_t padded_width_elements;
    std::uint32_t padded_height_elements;
    std::uint32_t padded_depth_or_array_size;
    std::uint32_t width_elements;
    std::uint32_t height_elements;
    std::uint32_t depth_or_array_size;
    std::uint32_t sample_count;
    std::uint32_t tile_mode;
    std::uint64_t bank_rotation_address_bit_mask;
    std::uint64_t bank_rotation_bytes_per_slice;
    std::uint32_t slice_depth_elements;
};
static_assert(sizeof(MipLayout) == 0x60);
static_assert(offsetof(MipLayout, tile_mode) == 0x40);
static_assert(offsetof(MipLayout, bank_rotation_address_bit_mask) == 0x48);

struct PlaneLayout {
    std::uint32_t usage;
    std::uint64_t size_bytes;
    std::uint64_t base_offset_bytes;
    std::uint64_t base_alignment_bytes;
    std::uint32_t bytes_per_element;
    MipLayout mips[kMaxMipLevels];
};
static_assert(offsetof(PlaneLayout, mips) == 0x28);
static_assert(sizeof(PlaneLayout) == 0x5C8);

struct ResourceLayout {
    std::uint64_t size_bytes;
    std::uint64_t base_alignment_bytes;
    std::uint32_t mip_levels;
    std::uint32_t planes;
    PlaneLayout plane[kMaxPlanes];
    std::uint32_t dimension;
};
static_assert(offsetof(ResourceLayout, plane) == 0x18);
static_assert(offsetof(ResourceLayout, dimension) == 0x1738);
static_assert(sizeof(ResourceLayout) == 0x1740);

struct FormatInfo {
    std::uint32_t bytes_per_element;
    std::uint32_t block_width;
    std::uint32_t block_height;
};

bool format_info(std::uint32_t format, FormatInfo* info);
std::uint32_t full_mip_chain(std::uint32_t width, std::uint32_t height);
HRESULT compute_linear_layout(const Texture2DDesc& desc, ResourceLayout* layout);
HRESULT compute_tiled_layout(const Texture2DDesc& desc, ResourceLayout* layout);
HRESULT compute_layout(const Texture2DDesc& desc, ResourceLayout* layout);

inline std::uint32_t optimal_tile_mode(std::uint32_t dimension, std::uint32_t samples) {
    return dimension == kResourceDimensionTexture2D && samples <= 1 ? kTileModeThin1D : kTileModeLinearGeneral;
}
std::uint64_t tiled_element_offset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch_elements,
                                   std::uint32_t bytes_per_element);
void detile(const unsigned char* tiled, std::uint32_t pitch_elements, std::uint32_t width_elements,
            std::uint32_t height_elements, std::uint32_t bytes_per_element, unsigned char* linear,
            std::uint64_t row_pitch);

}  // namespace g6shim::xg
