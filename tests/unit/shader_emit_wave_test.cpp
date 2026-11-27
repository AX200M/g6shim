// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "g6shim/shader/emit/wave.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;

TEST(wave_dpp_lanes_follow_the_isa) {
    const DppRow shift{0x111, 0xF, 0xF, true, false};
    CHECK(dpp_lane(shift, 16) == std::make_tuple(16u, true, true));
    CHECK(dpp_lane(shift, 17) == std::make_tuple(16u, false, true));
    const DppRow mirror{0x140, 0xF, 0xF, true, false};
    CHECK(dpp_lane(mirror, 3) == std::make_tuple(12u, false, true));
    const DppLanes swap{1u | 3u << 6 | 2u << 9 | 5u << 12 | 4u << 15 | 7u << 18 | 6u << 21, false};
    CHECK(dpp_lane(swap, 8) == std::make_tuple(9u, false, true));
}

TEST(wave_prefixes_are_recognised) {
    const Value x{1};
    const Value mask{2};
    const auto red = [&](std::uint64_t lanes) { return Sym::red(Op::AddU, x, mask, 0, lanes); };

    std::vector<Sym> inclusive;
    for (std::uint32_t lane = 0; lane < 64; ++lane) inclusive.push_back(red(UINT64_MAX >> (63 - lane)));
    CHECK(per_lane(inclusive, UINT64_MAX) == Shape::prefix(Op::AddU, x, mask, 0, true));

    std::vector<Sym> exclusive;
    for (std::uint32_t lane = 0; lane < 64; ++lane)
        exclusive.push_back(red((std::uint64_t{1} << lane) - 1));
    exclusive[0] = Sym::k(0);
    CHECK(per_lane(exclusive, UINT64_MAX) == Shape::prefix(Op::AddU, x, mask, 0, false));

    CHECK(per_lane(std::vector<Sym>(64, red(UINT64_MAX)), UINT64_MAX) ==
          Shape::active(Op::AddU, x, mask, 0));
}

constexpr std::uint32_t kEndpgm = 0xBF81'0000u;
constexpr std::uint32_t kInterpP1 = 0xC808'0000u;
constexpr std::uint32_t kInterpP2 = 0xC809'0001u;
constexpr std::uint32_t kQuadPerm = 0x7E06'02FAu;
constexpr std::uint32_t kQuadPermCtrl = 0xFF00'B102u;
constexpr std::uint32_t kSwizzle = 0xD8D4'80B1u;
constexpr std::uint32_t kSwizzleOps = 0x0300'0002u;
constexpr std::uint32_t kMbcnt = 0xD765'0000u;
constexpr std::uint32_t kMbcntSrc = 0x0001'00C1u;
constexpr std::uint32_t kMovFromS1 = 0x7E06'0201u;
constexpr std::uint32_t kXorLane = 0x3A02'0081u;
constexpr std::uint32_t kReadLane = 0xD760'0002u;
constexpr std::uint32_t kReadLaneSrc = 0x0202'0302u;
constexpr std::uint32_t kReadFirst = 0x7E02'0500u;
constexpr std::uint32_t kMovFromS = 0x7E06'0202u;
constexpr std::uint32_t kExport = 0xF800'0801u;
constexpr std::uint32_t kExportSrc = 0x0000'0003u;

bool bitcode_names_contain(const std::vector<std::uint8_t>& dxil, std::string_view text) {
    std::size_t at = 0;
    bool found_magic = false;
    for (; at + 4 <= dxil.size(); ++at) {
        if (dxil[at] == 'B' && dxil[at + 1] == 'C' && dxil[at + 2] == 0xC0 && dxil[at + 3] == 0xDE) {
            found_magic = true;
            break;
        }
    }
    if (!found_magic) return false;
    at += 4;
    std::vector<std::uint32_t> words;
    for (std::size_t i = at; i + 4 <= dxil.size(); i += 4) {
        words.push_back(static_cast<std::uint32_t>(dxil[i]) | static_cast<std::uint32_t>(dxil[i + 1]) << 8 |
                        static_cast<std::uint32_t>(dxil[i + 2]) << 16 |
                        static_cast<std::uint32_t>(dxil[i + 3]) << 24);
    }
    struct Bits {
        const std::vector<std::uint32_t>* words;
        std::size_t index = 0;
        std::uint64_t current = 0;
        int have = 0;
        std::uint32_t take(int width) {
            while (have < width && index < words->size()) {
                current |= static_cast<std::uint64_t>((*words)[index++]) << have;
                have += 32;
            }
            const std::uint32_t value = static_cast<std::uint32_t>(current & ((std::uint64_t{1} << width) - 1));
            current >>= width;
            have -= width;
            return value;
        }
        std::uint64_t vbr(int width) {
            const std::uint64_t high = std::uint64_t{1} << (width - 1);
            std::uint64_t value = 0;
            int shift = 0;
            for (;;) {
                const std::uint32_t chunk = take(width);
                value |= static_cast<std::uint64_t>(chunk & (high - 1)) << shift;
                if ((chunk & high) == 0) return value;
                shift += width - 1;
            }
        }
        void align() {
            current = 0;
            have = 0;
        }
        std::uint32_t word() {
            align();
            return index < words->size() ? (*words)[index++] : 0;
        }
    };
    Bits bits{&words};
    bool hit = false;
    const auto search = [&](auto&& self, int abbrev) -> void {
        if (hit) return;
        for (;;) {
            if (bits.index >= words.size() && bits.have < abbrev) return;
            const std::uint32_t id = bits.take(abbrev);
            if (id == 0) {
                bits.align();
                return;
            }
            if (id == 1) {
                (void)bits.vbr(8);
                const std::uint64_t next = bits.vbr(4);
                bits.align();
                (void)bits.word();
                self(self, static_cast<int>(next == 0 ? 2 : next));
                if (hit) return;
                continue;
            }
            if (id != 3) return;
            (void)bits.vbr(6);
            const std::uint64_t count = bits.vbr(6);
            std::string run;
            for (std::uint64_t n = 0; n < count; ++n) {
                const std::uint64_t op = bits.vbr(6);
                if (op >= 32 && op < 127)
                    run.push_back(static_cast<char>(op));
                else
                    run.clear();
                if (run.find(text) != std::string::npos) {
                    hit = true;
                    return;
                }
            }
        }
    };
    search(search, 2);
    return hit;
}

SigElement color_output() {
    SigElement element;
    element.name = "SV_Target";
    element.kind = 16;
    element.comp = 9;
    element.row = 0;
    element.cols = 1;
    return element;
}

SigElement texcoord_input() {
    SigElement element;
    element.name = "TEXCOORD";
    element.kind = 0;
    element.comp = 9;
    element.row = 1;
    element.cols = 1;
    return element;
}

Lowered lower_pixel(const std::vector<std::uint32_t>& words, bool interpolated) {
    NativeProgram native;
    native.stage = HwStage::Ps;
    native.wave64 = false;
    native.vgprs = 16;
    PsIo io;
    if (interpolated) {
        PsInput input;
        input.usage = Usage{Usage::Kind::Generic, 0};
        input.row = 1;
        input.mask = 1;
        input.attr = 0;
        input.row2 = 1;
        io.inputs.push_back(input);
        io.input_addr = 2;
    }
    native.io = io;
    native.code = g6shim::test::lowering::bytes_of(words);
    std::vector<SigElement> inputs;
    if (interpolated) inputs.push_back(texcoord_input());
    return g6shim::test::lowering::lower_native(native, inputs, {color_output()});
}

TEST(pixel_quad_perm_reads_the_quad_not_the_wave) {
    const Lowered lowered =
        lower_pixel({kInterpP1, kInterpP2, kQuadPerm, kQuadPermCtrl, kExport, kExportSrc, kEndpgm}, true);
    CHECK(bitcode_names_contain(lowered.dxil, "quadReadLaneAt"));
    CHECK(!bitcode_names_contain(lowered.dxil, "waveReadLaneAt"));
    g6shim::test::lowering::check_dxil(lowered.dxil, "pixel quad_perm");
}

TEST(pixel_quad_swizzle_reads_the_quad_not_the_wave) {
    const Lowered lowered =
        lower_pixel({kInterpP1, kInterpP2, kSwizzle, kSwizzleOps, kExport, kExportSrc, kEndpgm}, true);
    CHECK(bitcode_names_contain(lowered.dxil, "quadReadLaneAt"));
    CHECK(!bitcode_names_contain(lowered.dxil, "waveReadLaneAt"));
    g6shim::test::lowering::check_dxil(lowered.dxil, "pixel quad swizzle");
}

TEST(pixel_quad_local_readlane_uses_a_quad_read) {
    const Lowered lowered = lower_pixel(
        {kMbcnt, kMbcntSrc, kXorLane, kReadLane, kReadLaneSrc, kMovFromS, kExport, kExportSrc, kEndpgm},
        false);
    CHECK(bitcode_names_contain(lowered.dxil, "quadReadLaneAt"));
    CHECK(!bitcode_names_contain(lowered.dxil, "waveReadLaneAt"));
    g6shim::test::lowering::check_dxil(lowered.dxil, "pixel quad readlane");
}

TEST(pixel_helper_lane_reads_its_own_first_lane) {
    const Lowered lowered =
        lower_pixel({kReadFirst, kMovFromS1, kExport, kExportSrc, kEndpgm}, false);
    CHECK(bitcode_names_contain(lowered.dxil, "isHelperLane"));
    CHECK(bitcode_names_contain(lowered.dxil, "waveReadLaneFirst"));
    g6shim::test::lowering::check_dxil(lowered.dxil, "pixel helper first lane");
}
}  // namespace
