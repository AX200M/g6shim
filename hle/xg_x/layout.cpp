// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "layout.hpp"

#include <dxgiformat.h>

#include <algorithm>
#include <cstring>

namespace g6shim::xg {

namespace {

struct FormatRange {
    DXGI_FORMAT first;
    DXGI_FORMAT last;
    FormatInfo info;
};

constexpr FormatInfo kTexel1{1, 1, 1};
constexpr FormatInfo kTexel2{2, 1, 1};
constexpr FormatInfo kTexel4{4, 1, 1};
constexpr FormatInfo kTexel8{8, 1, 1};
constexpr FormatInfo kTexel12{12, 1, 1};
constexpr FormatInfo kTexel16{16, 1, 1};
constexpr FormatInfo kPairOfTexels4{4, 2, 1};
constexpr FormatInfo kBlock8{8, 4, 4};
constexpr FormatInfo kBlock16{16, 4, 4};

constexpr FormatRange kFormats[] = {
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT, kTexel16},
    {DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_SINT, kTexel12},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, kTexel8},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT, kTexel4},
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R16_SINT, kTexel2},
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_A8_UNORM, kTexel1},
    {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, kTexel4},
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kPairOfTexels4},
    {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, kBlock8},
    {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB, kBlock16},
    {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM, kBlock8},
    {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM, kBlock16},
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kTexel2},
    {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, kTexel4},
    {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB, kBlock16},
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kTexel2},
};

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint32_t elements(std::uint32_t texels, std::uint32_t block) { return (texels + block - 1) / block; }

}  // namespace

bool format_info(std::uint32_t format, FormatInfo* info) {
    for (const FormatRange& range : kFormats) {
        if (format >= static_cast<std::uint32_t>(range.first) && format <= static_cast<std::uint32_t>(range.last)) {
            *info = range.info;
            return true;
        }
    }
    return false;
}

std::uint32_t full_mip_chain(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    for (std::uint32_t size = std::max(width, height); size > 1; size >>= 1) ++levels;
    return levels;
}

HRESULT compute_linear_layout(const Texture2DDesc& desc, ResourceLayout* layout) {
    if (!layout) return E_POINTER;
    FormatInfo format{};
    if (desc.width == 0 || desc.height == 0 || !format_info(desc.format, &format)) return E_INVALIDARG;
    const std::uint32_t mips = desc.mip_levels ? desc.mip_levels : full_mip_chain(desc.width, desc.height);
    if (mips > kMaxMipLevels || mips > full_mip_chain(desc.width, desc.height)) return E_INVALIDARG;
    const std::uint32_t array_size = std::max(desc.array_size, 1u);
    const std::uint32_t samples = std::max(desc.sample_count, 1u);

    std::memset(layout, 0, sizeof *layout);
    PlaneLayout& plane = layout->plane[0];
    std::uint64_t offset = 0;
    for (std::uint32_t level = 0; level < mips; ++level) {
        MipLayout& mip = plane.mips[level];
        const std::uint32_t width = elements(std::max(desc.width >> level, 1u), format.block_width);
        const std::uint32_t height = elements(std::max(desc.height >> level, 1u), format.block_height);
        const auto pitch = static_cast<std::uint32_t>(align_up(std::uint64_t{width} * format.bytes_per_element,
                                                               kRowPitchAlignment));
        mip.slice_2d_size_bytes = align_up(std::uint64_t{pitch} * height * samples, kPlacementAlignment);
        mip.size_bytes = mip.slice_2d_size_bytes * array_size;
        mip.offset_bytes = offset;
        mip.pitch_bytes = pitch;
        mip.padded_width_elements = pitch / format.bytes_per_element;
        mip.pitch_pixels = mip.padded_width_elements * format.block_width;
        mip.alignment_bytes = kPlacementAlignment;
        mip.padded_height_elements = height;
        mip.padded_depth_or_array_size = array_size;
        mip.width_elements = width;
        mip.height_elements = height;
        mip.depth_or_array_size = array_size;
        mip.sample_count = samples;
        mip.tile_mode = kTileModeLinearGeneral;
        mip.slice_depth_elements = 1;
        offset += mip.size_bytes;
    }
    plane.usage = kPlaneUsageDefault;
    plane.size_bytes = offset;
    plane.base_alignment_bytes = kPlacementAlignment;
    plane.bytes_per_element = format.bytes_per_element;
    layout->size_bytes = offset;
    layout->base_alignment_bytes = kPlacementAlignment;
    layout->mip_levels = mips;
    layout->planes = 1;
    layout->dimension = kResourceDimensionTexture2D;
    return S_OK;
}

}  // namespace g6shim::xg
