// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "gfx10_layout.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using namespace g6shim::gfx10;

namespace {

Desc texture(std::uint32_t width, std::uint32_t height, std::uint32_t mips, std::uint32_t element_bytes,
             std::uint32_t swizzle) {
    Desc desc{};
    desc.dimension = Dimension::kTex2d;
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mips = mips;
    desc.element_bytes = element_bytes;
    desc.block = 1;
    desc.samples = 1;
    desc.swizzle = swizzle;
    return desc;
}

}  // namespace

TEST(gfx10_extents_past_the_hardware_limit_are_invalid) {
    const std::uint32_t swizzles[] = {kSwizzleLinear, kSwizzle64KBDisplayXor};
    const std::pair<std::uint32_t, std::uint32_t> extents[] = {
        {kMaxExtent + 1, 1},
        {1, kMaxExtent + 1},
        {0xFFFFFFFFu, 0xFFFFFFFFu},
    };
    for (std::uint32_t swizzle : swizzles) {
        const std::optional<Layout> largest = Layout::create(texture(kMaxExtent, kMaxExtent, 15, 16, swizzle));
        CHECK(largest.has_value());
        if (largest)
            CHECK(largest->size >= static_cast<std::uint64_t>(kMaxExtent) * kMaxExtent * 16);
        for (const auto& extent : extents)
            CHECK(!Layout::create(texture(extent.first, extent.second, 1, 16, swizzle)).has_value());
        Desc deep = texture(1, 1, 1, 16, swizzle);
        deep.depth = 0xFFFFFFFFu;
        CHECK(!Layout::create(deep).has_value());
    }
}

TEST(gfx10_blocks_hold_their_size_in_elements) {
    const std::uint32_t block_sizes[] = {8, 12, 16};
    for (std::uint32_t block_size : block_sizes) {
        const std::uint32_t swizzle =
            block_size == 8 ? kSwizzle256BStandard : block_size == 12 ? kSwizzle4KBStandard : kSwizzle64KBDisplay;
        for (std::uint32_t element = 0; element < 5; ++element) {
            const std::optional<Layout> layout =
                Layout::create(texture(1, 1, 1, std::uint32_t{1} << element, swizzle));
            CHECK(layout.has_value());
            if (!layout) continue;
            const std::uint64_t elements = static_cast<std::uint64_t>(layout->block[0]) * layout->block[1] *
                                           layout->block[2];
            CHECK((elements << element) == (std::uint64_t{1} << block_size));
            CHECK(layout->pattern[0].size() == layout->block[0]);
            CHECK(layout->pattern[1].size() == layout->block[1]);
        }
    }
    const std::optional<Layout> layout = Layout::create(texture(64, 64, 1, 16, kSwizzle64KBDisplay));
    CHECK(layout.has_value());
    if (!layout) return;
    CHECK(layout->block[0] == 64 && layout->block[1] == 64 && layout->block[2] == 1);
    CHECK(layout->size == 65536 && layout->alignment == 65536);
}

TEST(gfx10_addresses_are_a_bijection_and_copies_round_trip) {
    std::vector<Desc> cases;
    cases.push_back(texture(64, 64, 7, 16, kSwizzle64KBDisplay));
    cases.push_back(texture(100, 60, 7, 4, kSwizzle64KBDisplay));
    cases.push_back(texture(250, 175, 8, 8, kSwizzle4KBStandard));
    cases.push_back(texture(33, 17, 6, 1, kSwizzle4KBDisplay));
    cases.push_back(texture(40, 9, 3, 2, kSwizzle64KBStandard));
    cases.push_back(texture(30, 20, 5, 4, kSwizzleLinear));
    cases.push_back(texture(83, 31, 6, 1, kSwizzle256BStandard));
    Desc compressed = texture(109, 71, 6, 16, kSwizzle256BDisplay);
    compressed.block = 4;
    cases.push_back(compressed);
    Desc array = texture(70, 70, 4, 16, kSwizzle4KBStandard);
    array.depth = 3;
    cases.push_back(array);
    Desc volume = texture(64, 32, 7, 4, kSwizzle4KBStandard);
    volume.dimension = Dimension::kTex3d;
    volume.depth = 16;
    cases.push_back(volume);

    for (const Desc& desc : cases) {
        const std::optional<Layout> layout = Layout::create(desc);
        CHECK(layout.has_value());
        if (!layout) continue;
        std::vector<unsigned char> seen(static_cast<std::size_t>(layout->size), 0);
        std::vector<unsigned char> tiled(static_cast<std::size_t>(layout->size), 0);
        const std::size_t element_bytes = desc.element_bytes;
        const std::uint32_t slices = desc.dimension == Dimension::kTex3d ? 1 : desc.depth;
        for (std::uint32_t slice = 0; slice < slices; ++slice) {
            for (std::uint32_t mip = 0; mip < desc.mips; ++mip) {
                const Mip& level = layout->mips[mip];
                const std::size_t active = static_cast<std::size_t>(level.width) * element_bytes;
                const std::size_t row = active + 13;
                const std::size_t plane = row * level.height + 11;
                std::vector<unsigned char> linear(plane * level.depth);
                for (std::size_t i = 0; i < linear.size(); ++i)
                    linear[i] = static_cast<unsigned char>(i * 7 + mip * 13 + slice);
                CHECK(layout->tile(std::span<std::uint8_t>(tiled), mip, slice, std::span<const std::uint8_t>(linear),
                                   row, plane) == Error::kOk);
                std::vector<unsigned char> back(linear.size(), 0xcc);
                CHECK(layout->detile(std::span<const std::uint8_t>(tiled), mip, slice,
                                     std::span<std::uint8_t>(back), row, plane) == Error::kOk);
                for (std::uint32_t z = 0; z < level.depth; ++z) {
                    const std::size_t base = static_cast<std::size_t>(z) * plane;
                    for (std::uint32_t y = 0; y < level.height; ++y) {
                        const std::size_t start = base + static_cast<std::size_t>(y) * row;
                        bool same = true;
                        for (std::size_t i = 0; i < active; ++i) same = same && back[start + i] == linear[start + i];
                        CHECK(same);
                        bool untouched = true;
                        for (std::size_t i = active; i < row; ++i)
                            untouched = untouched && back[start + i] == 0xcc;
                        CHECK(untouched);
                        for (std::uint32_t x = 0; x < level.width; ++x) {
                            const OffsetResult at = layout->offset(mip, x, y, slice + z);
                            CHECK(at.error == Error::kOk);
                            const std::size_t offset = static_cast<std::size_t>(at.offset);
                            bool bytes_match = true;
                            for (std::size_t i = 0; i < element_bytes; ++i)
                                bytes_match = bytes_match && tiled[offset + i] == linear[start + x * element_bytes + i];
                            CHECK(bytes_match);
                            CHECK(offset + element_bytes <= seen.size());
                            CHECK(offset % element_bytes == 0);
                            CHECK(seen[offset] == 0);
                            seen[offset] = 1;
                        }
                    }
                    bool tail_clear = true;
                    for (std::size_t i = base + plane - 11; i < base + plane; ++i)
                        tail_clear = tail_clear && back[i] == 0xcc;
                    CHECK(tail_clear);
                }
            }
        }
    }
}

TEST(gfx10_hand_checked_addresses) {
    const std::optional<Layout> wide = Layout::create(texture(128, 128, 1, 4, kSwizzle64KBDisplay));
    CHECK(wide.has_value());
    if (!wide) return;
    CHECK(wide->offset(0, 1, 0, 0).offset == (1u << 2));
    CHECK(wide->offset(0, 4, 0, 0).offset == (1u << 6));
    CHECK(wide->offset(0, 0, 4, 0).offset == (1u << 7));
    CHECK(wide->offset(0, 0, 8, 0).offset == (1u << 8));
    CHECK(wide->offset(0, 64, 64, 0).offset == ((1u << 15) | (1u << 14)));

    const std::optional<Layout> small = Layout::create(texture(32, 32, 1, 16, kSwizzle4KBStandard));
    CHECK(small.has_value());
    if (!small) return;
    CHECK(small->block[0] == 16 && small->block[1] == 16 && small->block[2] == 1);
    CHECK(small->offset(0, 0, 1, 0).offset == (1u << 4));
    CHECK(small->offset(0, 1, 0, 0).offset == (1u << 6));
    CHECK(small->offset(0, 16, 0, 0).offset == 4096);
    CHECK(small->offset(0, 0, 16, 0).offset == 8192);
}

TEST(gfx10_mip_tail_comes_first) {
    const std::optional<Layout> tail = Layout::create(texture(64, 64, 9, 16, kSwizzle64KBDisplay));
    CHECK(tail.has_value());
    if (!tail) return;
    CHECK(tail->first_tail_mip == 1);
    CHECK(tail->size == 2 * 65536);
    CHECK(tail->mips[0].offset == 65536);
    CHECK(tail->mips[1].in_tail);
    CHECK(tail->mips[1].tail.offset == 32768 && tail->mips[1].tail.x == 32 && tail->mips[1].tail.y == 0);
    CHECK(tail->offset(1, 0, 0, 0).offset == 32768);

    const std::optional<Layout> small = Layout::create(texture(64, 64, 9, 16, kSwizzle4KBStandard));
    CHECK(small.has_value());
    if (!small) return;
    CHECK(small->first_tail_mip == 3);
    CHECK(small->mips[2].offset == 4096);
    CHECK(small->mips[1].offset == 8192);
    CHECK(small->mips[0].offset == 8192 + 16384);
}

TEST(gfx10_depth_stencil_is_two_planes) {
    TextureDesc desc{};
    desc.dimension = Dimension::kTex2d;
    desc.width = 1920;
    desc.height = 1080;
    desc.depth_or_array = 1;
    desc.mips = 1;
    desc.format = 20;
    desc.samples = 1;
    desc.swizzle = kSwizzle64KBZOrderXor;
    const std::optional<Texture> texture = Texture::create(desc);
    CHECK(texture.has_value());
    if (!texture) return;
    CHECK(texture->planes.size() == 2);
    CHECK(texture->planes[0].layout.size == 1920 * 1152 * 4);
    CHECK(texture->planes[1].offset == texture->planes[0].layout.size);
    CHECK(texture->planes[1].layout.size == 2048 * 1280);
    CHECK(texture->planes[0].layout.addressable());
}

TEST(gfx10_texture_copies_reject_overflowing_pitches) {
    const std::optional<Layout> layout = Layout::create(texture(4, 2, 1, 4, kSwizzleLinear));
    CHECK(layout.has_value());
    if (!layout) return;
    std::vector<unsigned char> tiled(static_cast<std::size_t>(layout->size), 0);
    unsigned char linear[32] = {};
    CHECK(layout->detile(std::span<const std::uint8_t>(tiled), 0, 0, std::span<std::uint8_t>(linear),
                         std::numeric_limits<std::size_t>::max(), 0) == Error::kTooSmall);
    CHECK(layout->tile(std::span<std::uint8_t>(tiled), 0, 0, std::span<const std::uint8_t>(linear),
                       std::numeric_limits<std::size_t>::max(), 0) == Error::kTooSmall);
}

TEST(gfx10_volume_copy_rejects_overflowing_depth_pitch) {
    Desc desc = texture(4, 2, 1, 4, kSwizzleLinear);
    desc.dimension = Dimension::kTex3d;
    desc.depth = 3;
    const std::optional<Layout> layout = Layout::create(desc);
    CHECK(layout.has_value());
    if (!layout) return;
    std::vector<unsigned char> tiled(static_cast<std::size_t>(layout->size), 0);
    unsigned char linear[96] = {};
    CHECK(layout->detile(std::span<const std::uint8_t>(tiled), 0, 0, std::span<std::uint8_t>(linear), 16,
                         std::numeric_limits<std::size_t>::max()) == Error::kTooSmall);
}

TEST(gfx10_bad_descriptions_and_buffers_are_refused) {
    CHECK(!Layout::create(texture(0, 4, 1, 4, kSwizzle4KBStandard)).has_value());
    CHECK(!Layout::create(texture(4, 4, 1, 12, kSwizzle4KBStandard)).has_value());
    CHECK(!Layout::create(texture(4, 4, 1, 4, 3)).has_value());

    const std::optional<Layout> layout = Layout::create(texture(64, 64, 1, 4, kSwizzle4KBStandard));
    CHECK(layout.has_value());
    if (!layout) return;
    std::vector<unsigned char> out(64 * 64 * 4, 0);
    const std::vector<unsigned char> tiny(100, 0);
    CHECK(layout->detile(std::span<const std::uint8_t>(tiny), 0, 0, std::span<std::uint8_t>(out), 256, 0) ==
          Error::kTooSmall);
    CHECK(layout->offset(0, 64, 0, 0).error == Error::kInvalid);
}
