// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "g6shim/shader/gfx10.hpp"
#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/error.hpp"
#include "g6shim/shader/ir.hpp"

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace g6shim::shader;

constexpr std::uint32_t kEndpgm = 0xBF81'0000;
constexpr std::uint32_t kBranch = 0xBF80'0000;

std::uint32_t sopp(std::uint32_t op, std::int16_t simm16) {
    return kBranch | op << 16 | static_cast<std::uint16_t>(simm16);
}

std::vector<std::uint8_t> bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> out;
    out.reserve(words.size() * 4);
    for (std::uint32_t word : words) {
        out.push_back(static_cast<std::uint8_t>(word));
        out.push_back(static_cast<std::uint8_t>(word >> 8));
        out.push_back(static_cast<std::uint8_t>(word >> 16));
        out.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return out;
}

std::vector<std::tuple<std::size_t, std::size_t, std::string>> summary(const std::vector<std::uint32_t>& code) {
    std::vector<std::tuple<std::size_t, std::size_t, std::string>> out;
    for (const Instruction& instruction : decode(bytes(code))) {
        out.emplace_back(instruction.offset, instruction.words.size(),
                         instruction.name() ? std::string(*instruction.name()) : std::string("?"));
    }
    return out;
}

std::vector<std::string> texts(const std::vector<std::uint32_t>& words, bool wave64) {
    std::vector<std::uint32_t> code = words;
    code.push_back(kEndpgm);
    const auto instructions = decode(bytes(code));
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < instructions.size(); ++i) out.push_back(instructions[i].text(wave64));
    return out;
}

std::string one(const std::vector<std::uint32_t>& words, bool wave64) {
    const auto result = texts(words, wave64);
    CHECK(result.size() == 1);
    return result.empty() ? std::string{} : result[0];
}

}  // namespace

TEST(shader_gfx10_tables_are_sorted) {
    for (Encoding encoding : kEncodings) {
        const auto table = opcode_table(encoding);
        for (std::size_t i = 1; i < table.size(); ++i) CHECK(table[i - 1].code < table[i].code);
    }
}

TEST(shader_gfx10_lengths_and_names) {
    const std::vector<std::uint32_t> code = {
        0xBE80'03FF,
        0x3F80'0000,
        0x0600'0000,
        0xD54B'0000,
        0x03FE'0000,
        0x4000'0000,
        0xF080'0F0A,
        0,
        0,
        0xF800'080F,
        0,
        kEndpgm,
    };
    const std::vector<std::tuple<std::size_t, std::size_t, std::string>> expected = {
        {0, 2, "s_mov_b32"}, {8, 1, "v_add_f32"},   {12, 3, "v_fma_f32"},
        {24, 3, "image_sample"}, {36, 2, "exp"},    {44, 1, "s_endpgm"},
    };
    CHECK(summary(code) == expected);
}

TEST(shader_gfx10_follows_branches_and_skips_data) {
    const std::vector<std::uint32_t> code = {
        sopp(0x04, 1),
        sopp(0x02, 1),
        kEndpgm,
        kEndpgm,
        kEndpgm,
        0xFFFF'FFFF,
    };
    const std::vector<std::size_t> expected = {0, 4, 8, 12};
    std::vector<std::size_t> found;
    for (const auto& item : summary(code)) found.push_back(std::get<0>(item));
    CHECK(found == expected);
}

TEST(shader_gfx10_follows_far_jumps) {
    const std::vector<std::uint32_t> code = {
        0xBE88'1F00,
        0x8008'FF08,
        20,
        0x8209'8009,
        0xBE80'2008,
        0xFFFF'FFFF,
        kEndpgm,
    };
    const std::vector<std::tuple<std::size_t, std::string>> expected = {
        {0, "s_getpc_b64"}, {4, "s_add_u32"}, {12, "s_addc_u32"}, {16, "s_setpc_b64"}, {24, "s_endpgm"},
    };
    std::vector<std::tuple<std::size_t, std::string>> found;
    for (const auto& item : summary(code)) found.emplace_back(std::get<0>(item), std::get<2>(item));
    CHECK(found == expected);
    CHECK(summary({0xBE80'2018, kEndpgm}).size() == 1);
}

TEST(shader_gfx10_follows_far_jumps_backward) {
    const std::vector<std::uint32_t> code = {
        sopp(0x02, 1),
        kEndpgm,
        0xBE88'1F00,
        0x8008'FF08,
        0xFFFF'FFF8,
        0x8209'C109,
        0xBE80'2008,
    };
    const std::vector<std::tuple<std::size_t, std::string>> expected = {
        {0, "s_branch"},   {4, "s_endpgm"},    {8, "s_getpc_b64"},
        {12, "s_add_u32"}, {20, "s_addc_u32"}, {24, "s_setpc_b64"},
    };
    std::vector<std::tuple<std::size_t, std::string>> found;
    for (const auto& item : summary(code)) found.emplace_back(std::get<0>(item), std::get<2>(item));
    CHECK(found == expected);
}

namespace {

bool decode_fails(const std::vector<std::uint32_t>& code) {
    try {
        decode(bytes(code));
    } catch (const ParseError&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(shader_gfx10_rejects_bad_code) {
    bool failed = false;
    try {
        decode(std::vector<std::uint8_t>{0, 0, 0});
    } catch (const ParseError&) {
        failed = true;
    }
    CHECK(failed);
    CHECK(decode_fails({sopp(0x02, 100)}));
    CHECK(decode_fails({0xBE80'03FF}));
    CHECK(decode_fails({0xE400'0000, 0}));
    CHECK(decode_fails({sopp(0x04, 1), 0xBE80'03FF, kEndpgm, kEndpgm}));
}

TEST(shader_gfx10_scalar_text) {
    const std::vector<std::string> expected = {
        "s_cbranch_execz label_0008",
        "s_waitcnt vmcnt(0) & lgkmcnt(0)",
        "s_mov_b32 s0, lit(0x12345678)",
    };
    CHECK(texts({0xBF88'0001, 0xBF8C'0070, 0xBE80'03FF, 0x1234'5678}, true) == expected);
    CHECK(one({0x8782'027E}, true) == "s_and_b64 s[2:3], exec, s[2:3]");
    CHECK(one({0xF408'0501, 0xFA00'0040}, true) == "s_load_dwordx4 s[20:23], s[2:3], 0x000040");
    CHECK(one({0xF420'0144, 0x0400'0000}, true) == "s_buffer_load_dword s5, s[8:11], s2");
}

TEST(shader_gfx10_lane_masks_follow_the_wave_size) {
    const std::vector<std::uint32_t> compare = {0x7C08'06F0};
    CHECK(one(compare, false) == "v_cmp_gt_f32 vcc_lo, 0.5, v3");
    CHECK(one(compare, true) == "v_cmp_gt_f32 vcc, 0.5, v3");
    const std::vector<std::uint32_t> vop3 = {0xD401'0106, 0x0001'E501};
    CHECK(one(vop3, false) == "v_cmp_lt_f32 s6, abs(v1), 1.0");
    CHECK(one(vop3, true) == "v_cmp_lt_f32 s[6:7], abs(v1), 1.0");
    CHECK(one({0x0202'0480}, false) == "v_cndmask_b32 v1, 0, v2, vcc_lo");
    CHECK(one({0x5006'0801}, true) == "v_add_co_ci_u32 v3, vcc, s1, v4, vcc");
}

TEST(shader_gfx10_vector_modifiers) {
    CHECK(one({0x1004'0AFF, 0x3F00'0001}, true) == "v_mul_f32 v2, lit(0x3f000001), v5");
    CHECK(one({0xD54B'8004, 0x2BC0'0501}, true) == "v_fma_f32 v4, -v1, s2, 0.5 mul:2 clamp");
    CHECK(one({0x7E02'16F9, 0x0605'0602}, true) == "v_cvt_f32_f16 v1, v2 src0_sel:WORD_1");
    CHECK(one({0x7E06'02E9, 0x0539'7701}, true) == "v_mov_b32 v3, v1 dpp8:[7,6,5,4,3,2,1,0]");
    CHECK(one({0x7E0A'02FA, 0xFF08'B106}, true) == "v_mov_b32 v5, v6 quad_perm:[1,0,3,2] bound_ctrl:1");
    CHECK(one({0xCC20'0C01, 0x2C10'0702}, true) == "v_fma_mix_f32 v1, -sel_hi(v2), s3, abs(v4)");
}

TEST(shader_gfx10_memory_and_export) {
    CHECK(one({0xE034'7010, 0x0102'0402}, true) ==
          "buffer_load_dwordx2 v[4:5], v[2:3], s[8:11], s1 idxen offen offset:16 glc");
    CHECK(one({0xD8DC'0302, 0x0600'0001}, true) == "ds_read2_b32 v[6:7], v1 offset0:2 offset1:3");
    CHECK(one({0xF800'1C0F, 0x0000'0302}, true) == "exp mrt0, v2, v2, v3, v3 done compr vm");
}

TEST(shader_gfx10_thread_trace_immediate_is_sopp_28) {
    const std::vector<std::uint32_t> code = {0xBFA8'1234, kEndpgm};
    const auto instructions = decode(bytes(code));
    CHECK(instructions.size() == 2);
    if (instructions.size() != 2) return;
    CHECK(instructions[0].name() == std::optional<std::string_view>{"s_ttracedata_imm"});
    const Program p = build(bytes(code), false, 1);
    CHECK(p.blocks.size() == 1);
}

TEST(shader_gfx10_image_d16_data_is_packed) {
    const std::vector<std::uint32_t> words = {0xF09C'070A, 0x8024'0801, 0x0000'0005};
    CHECK(one(words, true) ==
          "image_sample_lz v[8:9], [v1,v5], s[16:23], s[4:7] dmask:0x7 dim:SQ_RSRC_IMG_2D d16");
    std::vector<std::uint32_t> code = words;
    code.push_back(kEndpgm);
    const auto instructions = decode(bytes(code));
    CHECK(instructions.size() == 2);
    const auto image = std::get<ImageFields>(instructions[0].fields(true));
    CHECK((image.data == Operand{VectorOperand{8, 2}}));
    CHECK(image.addr.size() == 2);
}
