// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx10_layout.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace g6shim::gfx10 {

namespace {

constexpr std::array<std::array<std::uint32_t, 2>, 5> kBlock2562D{{
    {{16, 16}},
    {{16, 8}},
    {{8, 8}},
    {{8, 4}},
    {{4, 4}},
}};

constexpr std::array<std::array<std::uint32_t, 3>, 5> kBlock2563D{{
    {{8, 4, 8}},
    {{4, 4, 8}},
    {{4, 4, 4}},
    {{4, 2, 4}},
    {{2, 2, 4}},
}};

constexpr std::array<std::array<std::uint32_t, 3>, 5> kBlock1K3D{{
    {{16, 8, 8}},
    {{8, 8, 8}},
    {{8, 8, 4}},
    {{8, 4, 4}},
    {{4, 4, 4}},
}};

constexpr const char* kMicro[3][5] = {
    {
        "x0x1x2x3y0y1y2y3",
        "--x0x1x2y0y1y2x3",
        "----x0x1y0y1y2x2",
        "------x0y0y1x1x2",
        "--------y0y1x0x1",
    },
    {
        "x0x1x2y1y0y2x3y3",
        "--x0x1x2y0y1y2x3",
        "----x0x1y0y1x2y2",
        "------x0y0x1x2y1",
        "--------x0y0x1y1",
    },
    {
        "x0x1z0y0z1y1x2z2",
        "--x0z0y0z1y1x1z2",
        "----x0y0z0y1x1z1",
        "------x0z0y0x1z1",
        "--------z0y0x0z1",
    },
};

std::uint32_t divide_round_up(std::uint32_t value, std::uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

std::uint32_t round_up32(std::uint32_t value, std::uint32_t multiple) {
    return divide_round_up(value, multiple) * multiple;
}

std::uint64_t round_up64(std::uint64_t value, std::uint64_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

std::uint32_t dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
    return divide_round_up(std::max(texels >> mip, 1u), block);
}

std::uint32_t padded_dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) {
    return divide_round_up(divide_round_up(std::max(texels, 1u), block), 1u << mip);
}

std::array<std::vector<std::uint32_t>, 3> address_pattern(std::size_t kind, std::uint32_t element_log2,
                                                           std::uint32_t block_log2_value) {
    std::array<std::vector<std::uint32_t>, 3> axes;
    const char* micro = kMicro[kind][element_log2];
    for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const char axis = micro[2 * bit];
        std::size_t axis_index = 0;
        if (axis == 'x')
            axis_index = 0;
        else if (axis == 'y')
            axis_index = 1;
        else if (axis == 'z')
            axis_index = 2;
        else
            continue;
        const std::size_t index = static_cast<std::size_t>(micro[2 * bit + 1] - '0');
        if (axes[axis_index].size() <= index) axes[axis_index].resize(index + 1, 0);
        axes[axis_index][index] = bit;
    }
    constexpr std::array<std::size_t, 3> kThickOrder{1, 0, 2};
    constexpr std::array<std::size_t, 2> kThinOrder{1, 0};
    std::size_t position = 0;
    for (std::uint32_t bit = 8; bit < block_log2_value; ++bit, ++position) {
        const std::size_t axis = kind == 2 ? kThickOrder[position % 3] : kThinOrder[position % 2];
        axes[axis].push_back(bit);
    }
    return axes;
}

std::vector<std::uint32_t> spread(const std::vector<std::uint32_t>& bits) {
    const std::uint32_t count = 1u << bits.size();
    std::vector<std::uint32_t> offsets(count, 0);
    for (std::uint32_t coordinate = 0; coordinate < count; ++coordinate) {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < bits.size(); ++i)
            value |= ((coordinate >> i) & 1u) << bits[i];
        offsets[coordinate] = value;
    }
    return offsets;
}

}  // namespace

std::optional<Layout> Layout::create(const Desc& d) {
    const std::optional<std::uint32_t> log2 = block_log2(d.swizzle);
    if (!log2) return std::nullopt;
    const bool swizzle_fits_3d =
        d.swizzle == kSwizzleLinear || d.swizzle == kSwizzle4KBStandard || d.swizzle == kSwizzle64KBStandard ||
        d.swizzle == kSwizzle64KBStandardTiled || d.swizzle == kSwizzle4KBStandardXor ||
        (d.swizzle >= kSwizzle64KBZOrderXor && d.swizzle <= kSwizzle64KBRenderXor);
    const bool valid = d.width >= 1 && d.width <= kMaxExtent && d.height >= 1 && d.height <= kMaxExtent &&
                       d.depth >= 1 && d.depth <= kMaxExtent && d.mips >= 1 && d.mips <= kMaxMipLevels &&
                       std::has_single_bit(d.element_bytes) && d.element_bytes <= 16 &&
                       (d.block == 1 || d.block == 4) && std::has_single_bit(d.samples) && d.samples <= 8 &&
                       (d.samples == 1 || (d.dimension == Dimension::kTex2d && d.mips == 1)) &&
                       (d.dimension != Dimension::kTex1d || d.height == 1) &&
                       (d.dimension != Dimension::kTex3d || swizzle_fits_3d);
    if (!valid) return std::nullopt;
    if (*log2 == 0) return linear(d);
    return tiled(d, *log2);
}

Layout Layout::linear(const Desc& d) {
    const std::uint64_t element_bytes = d.element_bytes;
    const std::uint32_t align = 256 / d.element_bytes;
    const bool three_d = d.dimension == Dimension::kTex3d;
    std::vector<Mip> levels;
    levels.reserve(d.mips);
    std::uint64_t offset = 0;
    for (std::uint32_t i = d.mips; i-- > 0;) {
        const std::uint32_t height = padded_dim(d.height, i, d.block);
        const std::uint32_t pitch = round_up32(padded_dim(d.width, i, d.block), align);
        Mip level{};
        level.width = dim(d.width, i, d.block);
        level.height = dim(d.height, i, d.block);
        level.depth = three_d ? dim(d.depth, i, 1) : 1;
        level.pitch = pitch;
        level.rows = height;
        level.depth_pitch = three_d ? d.depth : 1;
        level.offset = offset;
        levels.push_back(level);
        offset += static_cast<std::uint64_t>(pitch) * height * element_bytes;
    }
    std::reverse(levels.begin(), levels.end());

    Layout layout{};
    layout.desc = d;
    layout.block = {align, 1, 1};
    layout.thick = false;
    layout.slices = d.depth;
    layout.slice_size = offset;
    layout.size = offset * d.depth;
    layout.alignment = 256;
    layout.first_tail_mip = d.mips;
    layout.mips = std::move(levels);
    return layout;
}

Layout Layout::tiled(const Desc& d, std::uint32_t block_log2_value) {
    const std::uint32_t element_log2 = static_cast<std::uint32_t>(std::countr_zero(d.element_bytes));
    const std::uint64_t element_bytes = d.element_bytes;
    const std::uint64_t block_bytes = std::uint64_t{1} << block_log2_value;
    const bool thick = d.dimension == Dimension::kTex3d && (is_standard(d.swizzle) || is_display(d.swizzle));
    std::array<std::uint32_t, 3> block{1, 1, 1};
    if (thick) {
        const std::uint32_t amp = (block_log2_value - 10) / 3;
        const std::uint32_t rest = (block_log2_value - 10) % 3;
        const auto& base = kBlock1K3D[element_log2];
        block = {base[0] << amp, base[1] << (amp + rest / 2), base[2] << (amp + (rest != 0 ? 1u : 0u))};
    } else {
        const std::uint32_t samples_log2 = static_cast<std::uint32_t>(std::countr_zero(d.samples));
        const std::uint32_t elements_log2 = block_log2_value - element_log2 - samples_log2;
        const bool wide = (samples_log2 & 1) == 0 || (block_log2_value & 1) != 0;
        const std::uint32_t width_log2 = (elements_log2 + (wide ? 1u : 0u)) / 2;
        block = {1u << width_log2, 1u << (elements_log2 - width_log2), 1};
    }

    const std::uint32_t slices = round_up32(d.depth, block[2]);
    const bool three_d = d.dimension == Dimension::kTex3d;
    std::vector<Mip> levels;
    levels.reserve(d.mips);
    std::uint32_t first_tail_mip = d.mips;
    std::uint64_t slice_size = 0;

    if (d.mips == 1) {
        Mip level{};
        level.width = dim(d.width, 0, d.block);
        level.height = dim(d.height, 0, d.block);
        level.depth = three_d ? dim(d.depth, 0, 1) : 1;
        level.pitch = round_up32(level.width, block[0]);
        level.rows = round_up32(level.height, block[1]);
        level.depth_pitch = three_d ? slices : 1;
        levels.push_back(level);
        slice_size = static_cast<std::uint64_t>(level.pitch) * level.rows * element_bytes * d.samples;
    } else {
        std::array<std::uint32_t, 3> tail_max = block;
        if (thick) {
            switch (block_log2_value % 3) {
            case 0: tail_max[1] >>= 1; break;
            case 1: tail_max[0] >>= 1; break;
            default: tail_max[2] >>= 1; break;
            }
        } else {
            tail_max[0] >>= 1;
        }
        const std::uint32_t effective =
            thick ? block_log2_value - (block_log2_value - 8) / 3 : block_log2_value;
        std::uint32_t max_in_tail = 0;
        if (effective == 8)
            max_in_tail = 0;
        else if (effective <= 11)
            max_in_tail = 1 + (1u << (effective - 9));
        else
            max_in_tail = effective - 4;

        std::array<std::uint32_t, 2> fixed{tail_max[0], tail_max[1]};
        if (is_z_order(d.swizzle) && element_log2 <= 1) {
            const auto& base = kBlock2562D[element_log2];
            fixed[0] /= base[0] / kBlock2562D[2][0];
            fixed[1] /= base[1] / kBlock2562D[2][1];
        }

        std::vector<std::uint64_t> sizes;
        for (std::uint32_t i = 0; i < d.mips; ++i) {
            const std::uint32_t width = padded_dim(d.width, i, d.block);
            const std::uint32_t height = padded_dim(d.height, i, d.block);
            if (width <= fixed[0] && height <= fixed[1] && d.mips - i <= max_in_tail) {
                first_tail_mip = i;
                slice_size += block_bytes / block[2];
                break;
            }
            const std::uint32_t pitch = round_up32(width, block[0]);
            const std::uint32_t rows = round_up32(height, block[1]);
            const std::uint32_t mip_depth = thick ? padded_dim(d.depth, i, 1) : 1;
            const std::uint32_t depth = round_up32(mip_depth, block[2]);
            const std::uint64_t one = static_cast<std::uint64_t>(pitch) * rows * element_bytes;
            slice_size += one;
            sizes.push_back(one * block[2]);
            Mip level{};
            level.width = dim(d.width, i, d.block);
            level.height = dim(d.height, i, d.block);
            level.depth = three_d ? dim(d.depth, i, 1) : 1;
            level.pitch = pitch;
            level.rows = rows;
            level.depth_pitch = three_d ? depth : 1;
            levels.push_back(level);
        }

        std::uint64_t offset = first_tail_mip < d.mips ? block_bytes : 0;
        for (std::uint32_t i = first_tail_mip; i-- > 0;) {
            levels[i].offset = offset;
            offset += sizes[i];
        }

        const std::uint32_t micro_width = thick ? kBlock2563D[element_log2][0] : kBlock2562D[element_log2][0];
        const std::uint32_t micro_height = thick ? kBlock2563D[element_log2][1] : kBlock2562D[element_log2][1];
        const std::uint32_t micro_depth = thick ? kBlock2563D[element_log2][2] : 1;
        std::uint32_t pitch = tail_max[0];
        std::uint32_t rows = tail_max[1];
        for (std::uint32_t i = first_tail_mip; i < d.mips; ++i) {
            const std::uint32_t position = max_in_tail - 1 - (i - first_tail_mip);
            const std::uint32_t tail_offset = position > 6 ? 16u << position : position << 8;
            const auto coordinate = [tail_offset](std::uint32_t from) {
                std::uint32_t value = 0;
                for (std::uint32_t b = 0; b < 6; ++b)
                    value |= ((tail_offset >> (from + 2 * b)) & 1u) << b;
                return value;
            };
            std::uint32_t x = coordinate(9);
            std::uint32_t y = coordinate(8);
            if ((block_log2_value & 1) != 0) {
                std::swap(x, y);
                if ((element_log2 & 1) != 0) {
                    y = (y << 1) | (x & 1u);
                    x >>= 1;
                }
            }
            Mip level{};
            level.width = dim(d.width, i, d.block);
            level.height = dim(d.height, i, d.block);
            level.depth = three_d ? dim(d.depth, i, 1) : 1;
            level.pitch = pitch;
            level.rows = rows;
            level.depth_pitch = thick ? round_up32(padded_dim(d.depth, i, 1), micro_depth) : 1;
            level.in_tail = true;
            level.tail = Tail{tail_offset, x * micro_width, y * micro_height};
            levels.push_back(level);
            pitch = std::max(pitch >> 1, micro_width);
            rows = std::max(rows >> 1, micro_height);
        }
    }

    const std::uint64_t slab = thick ? block[2] : 1;
    const std::uint64_t total_slice = slice_size * slab;
    const std::uint32_t slice_count = slices / block[2];
    const bool addressable = !is_xor(d.swizzle) && d.samples == 1;

    Layout layout{};
    layout.desc = d;
    layout.block = block;
    layout.thick = thick;
    layout.slices = slice_count;
    layout.slice_size = total_slice;
    layout.size = total_slice * slice_count;
    layout.alignment = block_bytes;
    layout.first_tail_mip = first_tail_mip;
    layout.mips = std::move(levels);
    if (addressable) {
        const std::size_t kind = thick ? 2 : (is_display(d.swizzle) ? 1 : 0);
        auto axes = address_pattern(kind, element_log2, block_log2_value);
        for (std::size_t axis = 0; axis < 3; ++axis) layout.pattern[axis] = spread(axes[axis]);
    }
    return layout;
}

bool Layout::addressable() const {
    return desc.swizzle == kSwizzleLinear || !pattern[0].empty();
}

std::pair<std::uint32_t, std::uint32_t> Layout::split(std::uint32_t z) const {
    if (thick) return {z / block[2], z % block[2]};
    return {z, 0};
}

std::uint64_t Layout::row(const Mip& level, std::uint32_t y, std::uint32_t z) const {
    const std::uint32_t block_width = block[0];
    const std::uint32_t block_height = block[1];
    const std::uint32_t align_log2 = static_cast<std::uint32_t>(std::countr_zero(alignment));
    y += level.in_tail ? level.tail.y : 0;
    const std::uint64_t blocks =
        static_cast<std::uint64_t>(y / block_height) * static_cast<std::uint64_t>(level.pitch / block_width);
    return (blocks << align_log2) +
           static_cast<std::uint64_t>(pattern[1][y % block_height] | pattern[2][z]);
}

std::uint64_t Layout::column(const Mip& level, std::uint32_t x) const {
    const std::uint32_t block_width = block[0];
    const std::uint32_t align_log2 = static_cast<std::uint32_t>(std::countr_zero(alignment));
    x += level.in_tail ? level.tail.x : 0;
    return (static_cast<std::uint64_t>(x / block_width) << align_log2) + pattern[0][x % block_width];
}

OffsetResult Layout::offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z) const {
    if (mip >= mips.size()) return {Error::kInvalid, 0};
    const Mip& level = mips[mip];
    const std::uint32_t depth = desc.dimension == Dimension::kTex3d ? level.depth : slices;
    const bool inside = x < level.width && y < level.height && z < depth;
    if (!inside || !addressable())
        return {inside ? Error::kUnsupported : Error::kInvalid, 0};
    const auto [target_slice, inner] = split(z);
    const std::uint64_t base = static_cast<std::uint64_t>(target_slice) * slice_size + level.offset;
    if (desc.swizzle == kSwizzleLinear) {
        const std::uint64_t linear_row = static_cast<std::uint64_t>(y) * level.pitch + x;
        return {Error::kOk, base + linear_row * desc.element_bytes};
    }
    return {Error::kOk, base + row(level, y, inner) + column(level, x)};
}

std::uint32_t Layout::subresource_depth(std::uint32_t mip) const {
    return mip < mips.size() ? mips[mip].depth : 0;
}

Error Layout::detile(std::span<const std::uint8_t> tiled, std::uint32_t mip, std::uint32_t slice,
                     std::span<std::uint8_t> linear, std::size_t row_pitch, std::size_t depth_pitch) const {
    return walk(tiled.size(), mip, slice, linear.size(), row_pitch, depth_pitch,
                [&](std::size_t from_tiled, std::size_t to_linear, std::size_t length) {
                    std::memcpy(linear.data() + to_linear, tiled.data() + from_tiled, length);
                });
}

Error Layout::tile(std::span<std::uint8_t> tiled, std::uint32_t mip, std::uint32_t slice,
                   std::span<const std::uint8_t> linear, std::size_t row_pitch, std::size_t depth_pitch) const {
    return walk(tiled.size(), mip, slice, linear.size(), row_pitch, depth_pitch,
                [&](std::size_t to_tiled, std::size_t from_linear, std::size_t length) {
                    std::memcpy(tiled.data() + to_tiled, linear.data() + from_linear, length);
                });
}

std::optional<Format> format(std::uint32_t dxgi) {
    struct Range {
        std::uint32_t first;
        std::uint32_t last;
        Format format;
    };
    constexpr Range kRanges[] = {
        {1, 4, {16, 1, false}},
        {9, 18, {8, 1, false}},
        {19, 22, {4, 1, true}},
        {44, 47, {4, 1, true}},
        {23, 43, {4, 1, false}},
        {67, 67, {4, 1, false}},
        {87, 93, {4, 1, false}},
        {116, 117, {4, 1, false}},
        {48, 59, {2, 1, false}},
        {85, 86, {2, 1, false}},
        {115, 115, {2, 1, false}},
        {118, 120, {2, 1, true}},
        {60, 65, {1, 1, false}},
        {70, 72, {8, 4, false}},
        {79, 81, {8, 4, false}},
        {73, 78, {16, 4, false}},
        {82, 84, {16, 4, false}},
        {94, 99, {16, 4, false}},
    };
    for (const Range& range : kRanges) {
        if (dxgi >= range.first && dxgi <= range.last) return range.format;
    }
    return std::nullopt;
}

std::optional<Texture> Texture::create(const TextureDesc& d) {
    const std::optional<Format> element = format(d.format);
    if (!element) return std::nullopt;
    const std::uint32_t depth_3d = d.dimension == Dimension::kTex3d ? d.depth_or_array : 1;
    const std::uint32_t largest = std::max({d.width, d.height, depth_3d, 1u});
    const std::uint32_t mips =
        d.mips == 0 ? 32u - static_cast<std::uint32_t>(std::countl_zero(largest)) : d.mips;
    const auto make_desc = [&](std::uint32_t bytes) {
        Desc out{};
        out.dimension = d.dimension;
        out.width = d.width;
        out.height = d.height;
        out.depth = d.depth_or_array;
        out.mips = mips;
        out.element_bytes = bytes;
        out.block = element->block;
        out.samples = std::max(d.samples, 1u);
        out.swizzle = d.swizzle;
        return out;
    };

    std::optional<Layout> base = Layout::create(make_desc(element->bytes));
    if (!base) return std::nullopt;
    Texture texture{};
    texture.planes.push_back(Plane{0, false, std::move(*base)});
    if (element->stencil) {
        std::optional<Layout> stencil = Layout::create(make_desc(1));
        if (!stencil) return std::nullopt;
        const std::uint64_t offset = round_up64(texture.planes[0].layout.size, stencil->alignment);
        texture.planes.push_back(Plane{offset, true, std::move(*stencil)});
    }
    const Plane& last = texture.planes.back();
    texture.size = last.offset + last.layout.size;
    std::uint64_t alignment = 1;
    for (const Plane& plane : texture.planes) alignment = std::max(alignment, plane.layout.alignment);
    texture.alignment = alignment;
    return texture;
}

}  // namespace g6shim::gfx10
