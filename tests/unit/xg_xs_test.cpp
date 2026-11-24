// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "texture_computer.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace g6shim::xg_xs;

namespace {

ResourceDesc texture(std::uint64_t width, std::uint32_t height, std::uint16_t mips, std::uint32_t format,
                     std::uint32_t layout) {
    ResourceDesc desc{};
    desc.dimension = kTexture2D;
    desc.width = width;
    desc.height = height;
    desc.depth_or_array = 1;
    desc.mips = mips;
    desc.format = format;
    desc.samples = 1;
    desc.layout = layout;
    return desc;
}

template <typename T>
T slot(const Computer* computer, std::size_t index) {
    return reinterpret_cast<T>(computer->vtable[index]);
}

using Release = std::uint32_t (*)(Computer*);
using GetLayoutFn = HRESULT (*)(Computer*, LayoutOut*);
using CopyInto = HRESULT (*)(Computer*, std::uint8_t*, std::uint32_t, std::uint32_t, const std::uint8_t*,
                             std::uint32_t, std::uint32_t);
using CopyFrom = HRESULT (*)(Computer*, std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t,
                             const std::uint8_t*, std::uint32_t, std::uint32_t);
using Tiling = HRESULT (*)(Computer*, std::uint32_t*, PackedMips*, std::array<std::uint32_t, 3>*, std::uint32_t*,
                           std::uint32_t, SubresourceTiling*);
using ElementOffset = std::uint64_t (*)(Computer*, std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t,
                                        std::uint32_t, std::uint32_t, std::uint32_t*);

}  // namespace

TEST(xg_xs_element_offsets_follow_the_layout) {
    const ResourceDesc desc = texture(128, 4, 1, 11, 0x100);
    Computer* computer = nullptr;
    CHECK(create_texture_computer(&desc, &computer) == S_OK && computer);
    if (!computer) return;
    const ElementOffset offset = slot<ElementOffset>(computer, 6);
    std::uint32_t bit = 7;
    CHECK(offset(computer, 0, 0, 3, 1, 0, 0, &bit) == (128 + 3) * 8);
    CHECK(bit == 0);
    CHECK(offset(computer, 0, 0, 128, 0, 0, 0, nullptr) == 0);
    CHECK(slot<Release>(computer, 1)(computer) == 0);
}

TEST(xg_xs_suggestions_follow_the_use) {
    std::uint32_t out = 0;
    ResourceDesc desc = texture(256, 256, 1, 98, 0);
    CHECK(suggest_swizzle_mode(&desc, 0, &out) == S_OK && out == 0x105);
    desc.flags = kAllowRenderTarget;
    CHECK(suggest_swizzle_mode(&desc, 0, &out) == S_OK && out == 0x11b);
    desc.flags = kAllowDepthStencil;
    CHECK(suggest_swizzle_mode(&desc, 0, &out) == S_OK && out == 0x118);
    CHECK(suggest_swizzle_mode(nullptr, 0, &out) == E_INVALIDARG);
    CHECK(suggest_swizzle_mode(&desc, 0, nullptr) == E_INVALIDARG);
}

TEST(xg_xs_layout_and_copies_through_the_object) {
    const ResourceDesc desc = texture(256, 256, 0, 98, 0x10a);
    Computer* computer = nullptr;
    CHECK(create_texture_computer(&desc, &computer) == S_OK && computer);
    if (!computer) return;

    LayoutOut layout{};
    CHECK(slot<GetLayoutFn>(computer, 2)(computer, &layout) == S_OK);
    CHECK(layout.size == 131072 && layout.alignment == 65536 && layout.mips == 9 && layout.planes == 1);
    const PlaneOut& plane = layout.plane[0];
    CHECK(plane.standard_mips == 1 && plane.tail_mips == 8 && plane.element_bytes == 16);
    CHECK(plane.mips[0].offset == 65536 && plane.mips[0].pitch_bytes == 1024);
    CHECK(plane.mips[1].offset == 32768 && plane.mips[1].in_tail == 1);

    const CopyInto into = slot<CopyInto>(computer, 8);
    const CopyFrom from = slot<CopyFrom>(computer, 9);
    std::vector<std::uint8_t> tiled(static_cast<std::size_t>(layout.size), 0);
    for (std::uint32_t mip = 0; mip < 9; ++mip) {
        const MipOut& level = plane.mips[mip];
        const std::uint32_t row = level.elements[0] * 16;
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(row) * level.elements[1]);
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<std::uint8_t>(i ^ mip);
        CHECK(into(computer, tiled.data(), 0, mip, rows.data(), row, 0) == S_OK);
        std::vector<std::uint8_t> back(rows.size(), 0);
        CHECK(from(computer, back.data(), 0, mip, 0, tiled.data(), row, 0) == S_OK);
        CHECK(back == rows);
    }

    const Tiling tiling = slot<Tiling>(computer, 10);
    std::uint32_t tiles = 0;
    std::array<std::uint32_t, 3> shape{};
    std::uint32_t count = 4;
    PackedMips packed{};
    SubresourceTiling subs[4] = {};
    CHECK(tiling(computer, &tiles, &packed, &shape, &count, 0, subs) == S_OK);
    CHECK(tiles == 2 && packed.standard == 1 && packed.packed == 8 && packed.tiles == 1);
    CHECK(shape[0] == 256 && shape[1] == 256 && shape[2] == 1 && count == 1 && subs[0].start == 1);
    CHECK(slot<Release>(computer, 1)(computer) == 0);

    const ResourceDesc small = texture(256, 256, 0, 98, 0x105);
    Computer* four = nullptr;
    CHECK(create_texture_computer(&small, &four) == S_OK && four);
    if (!four) return;
    CHECK(tiling(four, &tiles, nullptr, nullptr, nullptr, 0, nullptr) == E_FAIL);
    CHECK(slot<Release>(four, 1)(four) == 0);
}

TEST(xg_xs_bad_descriptions_fail) {
    Computer* computer = nullptr;
    ResourceDesc bad = texture(256, 256, 1, 0, 0x10a);
    CHECK(create_texture_computer(&bad, &computer) == E_INVALIDARG);
    bad = texture(256, 256, 1, 28, 0x103);
    CHECK(create_texture_computer(&bad, &computer) == E_INVALIDARG);
    CHECK(create_texture_computer(nullptr, &computer) == E_INVALIDARG);
    CHECK(create_texture_computer(&bad, nullptr) == E_INVALIDARG);
}

TEST(xg_xs_unimplemented_slots_report_failure) {
    const ResourceDesc desc = texture(128, 4, 1, 11, 0x100);
    Computer* computer = nullptr;
    CHECK(create_texture_computer(&desc, &computer) == S_OK && computer);
    if (!computer) return;
    using Unused = HRESULT (*)(Computer*);
    CHECK(slot<Unused>(computer, 3)(computer) == E_NOTIMPL);
    CHECK(slot<Unused>(computer, 7)(computer) == E_NOTIMPL);
    CHECK(slot<Release>(computer, 1)(computer) == 0);
}
