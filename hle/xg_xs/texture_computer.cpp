// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_computer.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace g6shim::xg_xs {

namespace {

constexpr std::uint32_t kRenderTailMode = 3;

std::uint32_t suggested(const ResourceDesc& desc) {
    if (desc.dimension == kTexture1D) return gfx10::kSwizzleLinear;
    if ((desc.flags & kAllowDepthStencil) != 0) return gfx10::kSwizzle64KBZOrderXor;
    if ((desc.flags & kAllowRenderTarget) != 0) return gfx10::kSwizzle64KBRenderXor;
    return gfx10::kSwizzle4KBStandard;
}

std::optional<gfx10::Texture> texture_from_desc(const ResourceDesc& desc) {
    gfx10::Dimension dimension = gfx10::Dimension::kTex2d;
    switch (desc.dimension) {
    case kTexture1D: dimension = gfx10::Dimension::kTex1d; break;
    case kTexture2D: dimension = gfx10::Dimension::kTex2d; break;
    case kTexture3D: dimension = gfx10::Dimension::kTex3d; break;
    default: return std::nullopt;
    }
    std::uint32_t swizzle = 0;
    if (desc.layout == 0)
        swizzle = suggested(desc);
    else if (desc.layout == kRowMajorLayout)
        swizzle = gfx10::kSwizzleLinear;
    else if ((desc.layout & ~0xFFu) == kSwizzledLayoutFlag)
        swizzle = desc.layout & 0xFFu;
    else
        return std::nullopt;
    if (desc.width > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;

    gfx10::TextureDesc texture_desc{};
    texture_desc.dimension = dimension;
    texture_desc.width = static_cast<std::uint32_t>(desc.width);
    texture_desc.height = desc.height;
    texture_desc.depth_or_array = desc.depth_or_array;
    texture_desc.mips = desc.mips;
    texture_desc.format = desc.format;
    texture_desc.samples = std::max(desc.samples, 1u);
    texture_desc.swizzle = swizzle;
    std::optional<gfx10::Texture> texture = gfx10::Texture::create(texture_desc);
    if (!texture || texture->planes.empty() || texture->planes[0].layout.mips.size() > kMaxComputerMips)
        return std::nullopt;
    return texture;
}

std::uint32_t tail_swizzle(std::uint32_t swizzle) {
    switch (swizzle) {
    case gfx10::kSwizzle256BDisplay:
    case gfx10::kSwizzle4KBDisplay:
    case gfx10::kSwizzle64KBDisplay:
    case gfx10::kSwizzle64KBDisplayTiled:
    case gfx10::kSwizzle4KBDisplayXor:
    case gfx10::kSwizzle64KBDisplayXor: return gfx10::kSwizzle256BDisplay;
    case gfx10::kSwizzle4KBRenderXor:
    case gfx10::kSwizzle64KBRenderXor: return kRenderTailMode;
    case gfx10::kSwizzle64KBZOrderXor: return swizzle;
    default: return gfx10::kSwizzle256BStandard;
    }
}

MipOut make_mip_out(const ResourceDesc& desc, const gfx10::Layout& layout, std::size_t index) {
    const gfx10::Mip& mip = layout.mips[index];
    const std::uint64_t element_bytes = layout.desc.element_bytes;
    const std::uint32_t block = layout.desc.block;
    const bool three_d = desc.dimension == kTexture3D;
    const std::uint32_t array = std::max<std::uint32_t>(desc.depth_or_array, 1);

    MipOut out{};
    out.pitch_texels = mip.pitch * block;
    out.pitch_bytes = mip.pitch * layout.desc.element_bytes;
    out.padded[0] = mip.pitch;
    out.padded[1] = mip.rows;
    out.padded[2] = three_d ? mip.depth_pitch : array;
    out.elements[0] = mip.width;
    out.elements[1] = mip.height;
    out.texels[0] = std::max(static_cast<std::uint32_t>(desc.width >> index), 1u);
    out.texels[1] = std::max(desc.height >> static_cast<std::uint32_t>(index), 1u);
    out.depth_or_array = three_d ? mip.depth : array;
    out.samples = layout.desc.samples;
    out.slice_depth = layout.block[2];

    const std::uint64_t slice = static_cast<std::uint64_t>(mip.pitch) * mip.rows * element_bytes;
    const std::uint64_t depth = three_d ? mip.depth_pitch : 1;
    if (!mip.in_tail) {
        out.offset = mip.offset;
        out.slice_size = slice * layout.desc.samples;
        out.alignment = static_cast<std::uint32_t>(layout.alignment);
        out.swizzle = layout.desc.swizzle;
    } else {
        out.in_tail = 1;
        out.alignment = kTailAlignment;
        out.swizzle = tail_swizzle(layout.desc.swizzle);
        if (layout.thick) {
            out.slice_size = layout.alignment / layout.block[2];
            out.tail_position = mip.tail.x | (mip.tail.y << 11);
        } else {
            out.offset = mip.tail.offset;
            out.slice_size = slice;
        }
    }
    out.size = out.slice_size * depth;
    return out;
}

std::uint32_t computer_add_ref(Computer* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t computer_release(Computer* self) {
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

HRESULT computer_layout(Computer* self, LayoutOut* out) {
    if (!out) return E_INVALIDARG;
    std::memset(out, 0, sizeof *out);
    out->size = self->texture.size;
    out->alignment = self->texture.alignment;
    out->mips = self->mips;
    out->planes = static_cast<std::uint32_t>(self->texture.planes.size());
    out->dimension = self->desc.dimension;
    const bool depth = (self->desc.flags & kAllowDepthStencil) != 0 &&
                       (self->texture.planes.size() > 1 || self->mips == 1);
    for (std::size_t i = 0; i < self->texture.planes.size() && i < kMaxComputerPlanes; ++i) {
        const gfx10::Plane& plane = self->texture.planes[i];
        const gfx10::Layout& layout = plane.layout;
        PlaneOut& target = out->plane[i];
        target.usage = plane.stencil ? kPlaneUsageStencil : (depth ? kPlaneUsageDepth : kPlaneUsageColor);
        target.size = layout.size;
        target.slice_size = layout.slice_size;
        target.offset = plane.offset;
        target.alignment = layout.alignment;
        target.element_bytes = layout.desc.element_bytes;
        target.standard_mips = layout.first_tail_mip;
        target.tail_mips = self->mips - layout.first_tail_mip;
        for (std::size_t m = 0; m < layout.mips.size() && m < kMaxComputerMips; ++m)
            target.mips[m] = make_mip_out(self->desc, layout, m);
    }
    return S_OK;
}

struct CopyArgs {
    const gfx10::Layout* layout;
    std::uint32_t mip;
    std::uint32_t slice;
    std::size_t tiled;
    std::size_t linear;
};

std::optional<CopyArgs> copy_args(const Computer& computer, std::uint32_t plane, std::uint32_t subresource,
                                  std::uint32_t row, std::uint32_t depth) {
    if (plane >= computer.texture.planes.size() || computer.mips == 0) return std::nullopt;
    const gfx10::Plane& plane_data = computer.texture.planes[plane];
    const gfx10::Layout& layout = plane_data.layout;
    const std::uint32_t mip = subresource % computer.mips;
    const std::uint32_t slice = subresource / computer.mips;
    if (mip >= layout.mips.size()) return std::nullopt;
    const gfx10::Mip& level = layout.mips[mip];
    const std::size_t bytes = static_cast<std::size_t>(level.width) * layout.desc.element_bytes * layout.desc.samples;
    const std::size_t depths = layout.subresource_depth(mip);
    const std::size_t linear =
        (depths - 1) * depth + (static_cast<std::size_t>(level.height) - 1) * row + bytes;
    return CopyArgs{&layout, mip, slice, static_cast<std::size_t>(plane_data.offset + layout.size), linear};
}

HRESULT result(gfx10::Error error) {
    switch (error) {
    case gfx10::Error::kOk: return S_OK;
    case gfx10::Error::kUnsupported: return E_NOTIMPL;
    case gfx10::Error::kInvalid:
    case gfx10::Error::kTooSmall: return E_INVALIDARG;
    }
    return E_INVALIDARG;
}

HRESULT computer_copy_into(Computer* self, std::uint8_t* tiled, std::uint32_t plane, std::uint32_t subresource,
                           const std::uint8_t* linear, std::uint32_t row, std::uint32_t depth) {
    const std::optional<CopyArgs> args = copy_args(*self, plane, subresource, row, depth);
    if (!args) return E_INVALIDARG;
    if (!tiled || !linear) return E_INVALIDARG;
    const std::size_t offset = static_cast<std::size_t>(self->texture.planes[plane].offset);
    const std::size_t length = args->tiled - offset;
    const gfx10::Error error =
        args->layout->tile(std::span<std::uint8_t>(tiled + offset, length), args->mip, args->slice,
                           std::span<const std::uint8_t>(linear, args->linear), row, depth);
    if (error != gfx10::Error::kOk)
        G6SHIM_WARN("xg_xs: copy into a texture failed (error %u, subresource %u)", static_cast<unsigned>(error),
                    subresource);
    return result(error);
}

HRESULT computer_copy_from(Computer* self, std::uint8_t* linear, std::uint32_t plane, std::uint32_t subresource,
                           std::uint32_t, const std::uint8_t* tiled, std::uint32_t row, std::uint32_t depth) {
    const std::optional<CopyArgs> args = copy_args(*self, plane, subresource, row, depth);
    if (!args) return E_INVALIDARG;
    if (!tiled || !linear) return E_INVALIDARG;
    const std::size_t offset = static_cast<std::size_t>(self->texture.planes[plane].offset);
    const std::size_t length = args->tiled - offset;
    const gfx10::Error error =
        args->layout->detile(std::span<const std::uint8_t>(tiled + offset, length), args->mip, args->slice,
                             std::span<std::uint8_t>(linear, args->linear), row, depth);
    if (error != gfx10::Error::kOk)
        G6SHIM_WARN("xg_xs: copy out of a texture failed (error %u, subresource %u)",
                    static_cast<unsigned>(error), subresource);
    return result(error);
}

HRESULT computer_tiling(Computer* self, std::uint32_t* tiles, PackedMips* packed,
                        std::array<std::uint32_t, 3>* shape, std::uint32_t* count, std::uint32_t first,
                        SubresourceTiling* out) {
    const gfx10::Layout& layout = self->texture.planes[0].layout;
    if (layout.alignment != kTileBytes) return E_FAIL;
    if (layout.thick) return E_NOTIMPL;

    const std::uint32_t standard = layout.first_tail_mip;
    const bool tail = standard < self->mips;
    std::uint64_t tail_tiles = 0;
    if (standard == 0)
        tail_tiles = layout.slice_size / kTileBytes;
    else if (standard < self->mips)
        tail_tiles = layout.mips[standard - 1].offset / kTileBytes;

    const std::uint32_t block = layout.desc.block;
    if (tiles) *tiles = static_cast<std::uint32_t>(layout.size / kTileBytes);
    if (packed) {
        *packed = PackedMips{static_cast<std::uint8_t>(standard), static_cast<std::uint8_t>(self->mips - standard),
                             static_cast<std::uint32_t>(tail_tiles), 0};
    }
    if (shape) *shape = {layout.block[0] * block, layout.block[1] * block, layout.block[2]};
    if (count) {
        const std::uint32_t available = *count;
        const std::uint32_t slices = tail ? std::min(layout.slices, 2u) : layout.slices;
        const std::uint32_t total = standard * slices;
        const std::uint32_t wanted = std::min(total > first ? total - first : 0u, available);
        for (std::uint32_t i = 0; i < wanted; ++i) {
            const std::uint32_t slice = (first + i) / standard;
            const std::uint32_t mip = (first + i) % standard;
            const gfx10::Mip& level = layout.mips[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry.width = level.pitch / layout.block[0];
                entry.height = static_cast<std::uint16_t>(level.rows / layout.block[1]);
                entry.depth = 1;
                entry.start = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(slice) * layout.slice_size + level.offset) / kTileBytes);
            }
            out[i] = entry;
        }
        *count = wanted;
    }
    return S_OK;
}

std::uint64_t computer_element_offset(Computer* self, std::uint32_t plane, std::uint32_t mip, std::uint64_t x,
                                      std::uint32_t y, std::uint32_t z, std::uint32_t sample, std::uint32_t* bit) {
    if (bit) *bit = 0;
    if (plane >= self->texture.planes.size()) return 0;
    const gfx10::Plane& plane_data = self->texture.planes[plane];
    const std::uint32_t x32 =
        x > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                      : static_cast<std::uint32_t>(x);
    const gfx10::OffsetResult at = plane_data.layout.offset(mip, x32, y, z, sample);
    if (at.error == gfx10::Error::kOk) return plane_data.offset + at.offset;
    G6SHIM_WARN("xg_xs: texel outside its texture (error %u, mip %u, x %u, y %u, z %u)",
                static_cast<unsigned>(at.error), mip, x32, y, z);
    return 0;
}

HRESULT computer_unused(Computer* self) {
    G6SHIM_WARN("xg_xs: texture computer method not implemented (width %llu, height %u, layout 0x%X)",
                static_cast<unsigned long long>(self->desc.width), self->desc.height, self->desc.layout);
    return E_NOTIMPL;
}

const void* const* vtable() {
    static const std::array<const void*, 13> slots = {{
        reinterpret_cast<const void*>(&computer_add_ref),
        reinterpret_cast<const void*>(&computer_release),
        reinterpret_cast<const void*>(&computer_layout),
        reinterpret_cast<const void*>(&computer_unused),
        reinterpret_cast<const void*>(&computer_unused),
        reinterpret_cast<const void*>(&computer_unused),
        reinterpret_cast<const void*>(&computer_element_offset),
        reinterpret_cast<const void*>(&computer_unused),
        reinterpret_cast<const void*>(&computer_copy_into),
        reinterpret_cast<const void*>(&computer_copy_from),
        reinterpret_cast<const void*>(&computer_tiling),
        reinterpret_cast<const void*>(&computer_unused),
        reinterpret_cast<const void*>(&computer_unused),
    }};
    return slots.data();
}

}  // namespace

HRESULT create_texture_computer(const ResourceDesc* desc, Computer** out) {
    if (!desc || !out) return E_INVALIDARG;
    std::optional<gfx10::Texture> texture = texture_from_desc(*desc);
    if (!texture) {
        G6SHIM_WARN("xg_xs: no texture layout (width %llu, height %u, format %u, layout 0x%X)",
                    static_cast<unsigned long long>(desc->width), desc->height, desc->format, desc->layout);
        return E_INVALIDARG;
    }
    G6SHIM_TRACE("xg_xs: texture computer (width %llu, height %u, format %u, layout 0x%X, size %llu)",
                 static_cast<unsigned long long>(desc->width), desc->height, desc->format, desc->layout,
                 static_cast<unsigned long long>(texture->size));
    auto* computer = new (std::nothrow) Computer{};
    if (!computer) return E_OUTOFMEMORY;
    computer->vtable = vtable();
    computer->refs.store(1, std::memory_order_relaxed);
    computer->desc = *desc;
    computer->mips = static_cast<std::uint32_t>(texture->planes[0].layout.mips.size());
    computer->texture = std::move(*texture);
    *out = computer;
    return S_OK;
}

HRESULT suggest_swizzle_mode(const ResourceDesc* desc, std::uint32_t, std::uint32_t* out) {
    if (!desc || !out) return E_INVALIDARG;
    *out = kSwizzledLayoutFlag | suggested(*desc);
    return S_OK;
}

}  // namespace g6shim::xg_xs
