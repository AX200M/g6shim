// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "gfx10_layout.hpp"

#include <cstdint>
#include <span>
#include <vector>

using namespace g6shim::gfx10;

namespace {

AddressConfig tested_config(std::uint32_t pipes_log2, std::uint32_t packers_log2, std::uint32_t fragments_log2) {
    AddressConfig config{};
    config.pipes = 1u << pipes_log2;
    config.packers = 1u << packers_log2;
    config.pipe_interleave = kDefaultPipeInterleave;
    config.max_compressed_fragments = 1u << fragments_log2;
    return config;
}

Desc tested_desc(std::uint32_t pipes_log2, std::uint32_t packers_log2, std::uint32_t fragments_log2,
                 std::uint32_t swizzle, std::uint32_t dimension, std::uint32_t element_log2, std::uint32_t samples,
                 std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t mips) {
    Desc desc{};
    desc.dimension = dimension == 1 ? Dimension::kTex1d : dimension == 3 ? Dimension::kTex3d : Dimension::kTex2d;
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.mips = mips;
    desc.element_bytes = 1u << element_log2;
    desc.block = 1;
    desc.samples = samples;
    desc.swizzle = swizzle;
    desc.address = tested_config(pipes_log2, packers_log2, fragments_log2);
    return desc;
}

#include "gfx10_reference.inc"

}  // namespace

void gfx10_xor_addresses_match_the_reference_body() {
    for (const ReferenceSize& row : kReferenceSizes) {
        const std::optional<Layout> layout =
            Layout::create(tested_desc(row.pipes_log2, row.packers_log2, row.fragments_log2, row.swizzle,
                                       row.dimension, row.element_log2, row.samples, row.width, row.height, row.depth,
                                       row.mips));
        CHECK(layout.has_value());
        if (!layout) continue;
        CHECK(layout->size == row.bytes);
        CHECK(layout->block[0] == row.block_w && layout->block[1] == row.block_h && layout->block[2] == row.block_d);
        CHECK(layout->alignment == row.alignment);
        CHECK(layout->first_tail_mip == row.first_tail);
        CHECK(layout->addressable());
    }
    for (const ReferenceMip& row : kReferenceMips) {
        const std::optional<Layout> layout =
            Layout::create(tested_desc(row.pipes_log2, row.packers_log2, row.fragments_log2, row.swizzle,
                                       row.dimension, row.element_log2, row.samples, row.width, row.height, row.depth,
                                       row.mips));
        CHECK(layout.has_value());
        if (!layout || row.mip >= layout->mips.size()) continue;
        const Mip& level = layout->mips[row.mip];
        if (row.in_tail) {
            CHECK(level.in_tail);
            CHECK(level.tail.offset == row.tail_offset);
            CHECK(level.tail.x == row.tail_x && level.tail.y == row.tail_y);
        } else {
            CHECK(level.offset == row.offset);
        }
    }
    for (const ReferenceAddress& row : kReferenceAddresses) {
        const std::optional<Layout> layout =
            Layout::create(tested_desc(row.pipes_log2, row.packers_log2, row.fragments_log2, row.swizzle,
                                       row.dimension, row.element_log2, row.samples, row.width, row.height, row.depth,
                                       row.mips));
        CHECK(layout.has_value());
        if (!layout) continue;
        const OffsetResult at = layout->offset(row.mip, row.x, row.y, row.z, row.sample);
        CHECK(at.error == Error::kOk);
        CHECK(at.offset == row.offset);
    }
}

void gfx10_xor_and_multisample_copies_round_trip_body() {
    Desc desc = tested_desc(4, 3, 3, kSwizzle64KBStandardXor, 2, 2, 1, 48, 36, 1, 4);
    Desc volume = tested_desc(3, 2, 2, kSwizzle64KBZOrderXor, 3, 2, 1, 24, 16, 8, 3);
    Desc sampled = tested_desc(2, 1, 1, kSwizzle64KBRenderXor, 2, 2, 4, 20, 12, 1, 1);
    const Desc cases[] = {desc, volume, sampled};
    for (const Desc& item : cases) {
        const std::optional<Layout> layout = Layout::create(item);
        CHECK(layout.has_value());
        if (!layout) continue;
        std::vector<unsigned char> tiled(static_cast<std::size_t>(layout->size), 0);
        for (std::uint32_t mip = 0; mip < item.mips; ++mip) {
            const Mip& level = layout->mips[mip];
            const std::size_t row = static_cast<std::size_t>(level.width) * item.element_bytes * item.samples + 8;
            const std::size_t plane = row * level.height + 4;
            const std::size_t used = static_cast<std::size_t>(level.width) * item.element_bytes * item.samples;
            std::vector<unsigned char> linear(plane * level.depth, 0x5a);
            for (std::uint32_t z = 0; z < level.depth; ++z) {
                for (std::uint32_t y = 0; y < level.height; ++y) {
                    const std::size_t at = static_cast<std::size_t>(z) * plane + static_cast<std::size_t>(y) * row;
                    for (std::size_t i = 0; i < used; ++i)
                        linear[at + i] = static_cast<unsigned char>(i * 3 + mip + z);
                }
            }
            CHECK(layout->tile(std::span<std::uint8_t>(tiled), mip, 0, std::span<const std::uint8_t>(linear), row,
                               plane) == Error::kOk);
            std::vector<unsigned char> back(linear.size(), 0x5a);
            CHECK(layout->detile(std::span<const std::uint8_t>(tiled), mip, 0, std::span<std::uint8_t>(back), row,
                                 plane) == Error::kOk);
            CHECK(back == linear);
        }
    }
}

void gfx10_unpublished_xor_equations_stay_unaddressable_body() {
    Desc wide = tested_desc(4, 3, 3, kSwizzle64KBStandardXor, 2, 2, 1, 32, 32, 1, 1);
    wide.address.pipe_interleave = 512;
    const std::optional<Layout> layout = Layout::create(wide);
    CHECK(layout.has_value());
    if (layout) CHECK(!layout->addressable());
    Desc render = tested_desc(4, 3, 3, kSwizzle4KBRenderXor, 2, 2, 1, 32, 32, 1, 1);
    const std::optional<Layout> missing = Layout::create(render);
    CHECK(missing.has_value());
    if (missing) CHECK(!missing->addressable());
    Desc bad = tested_desc(4, 3, 3, kSwizzle64KBStandardXor, 2, 2, 1, 32, 32, 1, 1);
    bad.address.packers = 1;
    CHECK(!Layout::create(bad).has_value());
}
