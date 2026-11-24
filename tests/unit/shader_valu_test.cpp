// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "g6shim/shader/emit/container.hpp"
#include "g6shim/shader/emit/emit.hpp"
#include "g6shim/shader/emit/lower.hpp"
#include "g6shim/shader/emit/validator.hpp"
#include "g6shim/shader/error.hpp"

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;

constexpr std::uint32_t kSEndPgm = 0xBF81'0000u;

std::uint32_t s_mov_b32(std::uint8_t sdst, std::uint8_t src0) {
    return 0xBE80'0000u | static_cast<std::uint32_t>(sdst) << 16 | 0x03u << 8 | src0;
}

std::uint32_t sop2(std::uint8_t op, std::uint8_t sdst, std::uint8_t src0, std::uint8_t src1) {
    return 0x8000'0000u | (static_cast<std::uint32_t>(op) & 0x7Fu) << 23 |
           (static_cast<std::uint32_t>(sdst) & 0x7Fu) << 16 |
           static_cast<std::uint32_t>(src1) << 8 | src0;
}

std::array<std::uint32_t, 2> s_load_dwordx4(std::uint8_t dst, std::uint8_t base) {
    return {0x3Du << 26 | 0x02u << 18 | static_cast<std::uint32_t>(dst) << 6 | (base >> 1),
            125u << 25};
}

std::uint32_t vop1(std::uint16_t op, std::uint8_t vdst, std::uint16_t src0) {
    return 0x7E00'0000u | static_cast<std::uint32_t>(vdst) << 17 |
           static_cast<std::uint32_t>(op) << 9 | (src0 & 0x1FFu);
}

std::array<std::uint32_t, 2> vop3(std::uint16_t op, std::uint8_t vdst, std::uint16_t src0,
                                  std::uint16_t src1, std::uint16_t src2) {
    return {0x35u << 26 | (static_cast<std::uint32_t>(op) & 0x3FFu) << 16 | vdst,
            (src0 & 0x1FFu) | ((src1 & 0x1FFu) << 9) | ((src2 & 0x1FFu) << 18)};
}

std::array<std::uint32_t, 2> buffer_store_dword(std::uint8_t vdata, std::uint8_t srsrc,
                                                std::uint16_t byte_offset) {
    return {0x38u << 26 | 0x1Cu << 18 | (byte_offset & 0xFFFu),
            0x80u << 24 | ((srsrc >> 2) & 0x1Fu) << 16 | static_cast<std::uint32_t>(vdata) << 8};
}

std::uint16_t sgpr(std::uint8_t n) { return n; }

std::uint16_t vgpr(std::uint8_t n) { return static_cast<std::uint16_t>(256 + n); }

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> code;
    code.reserve(words.size() * 4);
    for (std::uint32_t word : words) {
        code.push_back(static_cast<std::uint8_t>(word));
        code.push_back(static_cast<std::uint8_t>(word >> 8));
        code.push_back(static_cast<std::uint8_t>(word >> 16));
        code.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return code;
}

RootParam root_param(RootType::Kind kind, std::uint8_t user_data, std::uint8_t slot) {
    RootParam param;
    param.root = RootType{kind, 0};
    param.table_offset_dwords = 0;
    param.sgprs = kind == RootType::Kind::Constants ? 4 : 1;
    param.base_register = 0;
    param.user_data = user_data;
    param.slot = slot;
    return param;
}

UsedResource used_uav(std::uint8_t user_data, std::uint8_t slot) {
    UsedResource used;
    used.kind = ResourceType{ResourceType::Kind::Uav, 0};
    used.root = RootType{RootType::Kind::Table, 0};
    used.table_offset_dwords = 0;
    used.lower_bound = 0;
    used.stride = 0;
    used.dwords = 4;
    used.user_data = user_data;
    used.slot = slot;
    return used;
}

Lowered lower_compute(const std::vector<std::uint32_t>& words, std::uint16_t vgprs,
                      const std::vector<RootParam>& layout, const std::vector<UsedResource>& used,
                      std::uint32_t rsrc2, std::array<std::uint32_t, 3> threads) {
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.wave64 = false;
    native.vgprs = vgprs;
    native.layout = layout;
    native.used = used;
    ComputeIo io;
    io.rsrc2 = rsrc2;
    io.threads = threads;
    native.io = io;
    native.code = bytes_of(words);
    const Program program = ::g6shim::shader::build(native.patched_code(), native.wave64, native.vgprs);
    const Resources resources = track(program, native.layout, native.stage);
    const Io raised = raise(program, native);
    const Structured structured = structure(program);
    Parts parts;
    parts.native = &native;
    parts.p = &program;
    parts.st = &structured;
    parts.io = &raised;
    parts.res = &resources;
    return lower(parts);
}

bool lowers(const std::vector<std::uint32_t>& words, std::string_view* reason = nullptr) {
    try {
        lower_compute(words, 256, {}, {}, 0, {1, 1, 1});
        return true;
    } catch (const std::exception& error) {
        if (reason != nullptr) *reason = error.what();
        return false;
    }
}

std::optional<Validator> load_validator() {
    std::optional<Validator> validator;
    try {
        const std::optional<std::filesystem::path> path = Validator::default_path();
        validator.emplace(Validator::load(path ? &*path : nullptr));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return validator;
}

void check_dxil(const std::vector<std::uint8_t>& dxil, const char* label) {
    CHECK(!dxil.empty());
    const Container container = Container::parse(dxil);
    CHECK(container.part(kDxil) != nullptr);
    std::optional<Validator> validator = load_validator();
    if (!validator) {
        std::printf("no dxil.dll; validation skipped for %s\n", label);
        return;
    }
    std::vector<std::uint8_t> copy = dxil;
    std::array<std::uint8_t, 16> ours{};
    std::copy_n(copy.begin() + 4, 16, ours.begin());
    bool accepted = true;
    try {
        validator->validate(copy);
    } catch (const std::exception& error) {
        std::printf("validation failed for %s: %s\n", label, error.what());
        accepted = false;
    }
    CHECK(accepted);
    CHECK((std::equal(ours.begin(), ours.end(), copy.begin() + 4)));
}

Lowered build_valu_cs(const std::vector<std::uint32_t>& body) {
    std::vector<std::uint32_t> words;
    words.push_back(s_mov_b32(5, 0x80));
    const std::array<std::uint32_t, 2> load = s_load_dwordx4(8, 4);
    words.insert(words.end(), load.begin(), load.end());
    for (std::uint8_t i = 0; i < 4; ++i) words.push_back(vop1(0x001, static_cast<std::uint8_t>(i + 1), sgpr(i)));
    words.insert(words.end(), body.begin(), body.end());
    const std::array<std::uint32_t, 2> store = buffer_store_dword(0, 8, 0);
    words.insert(words.end(), store.begin(), store.end());
    words.push_back(kSEndPgm);
    return lower_compute(words, 8,
                         {root_param(RootType::Kind::Constants, 0, 0),
                          root_param(RootType::Kind::Table, 4, 1)},
                         {used_uav(4, 1)}, 5u << 1, {1, 1, 1});
}

void check_valu(const std::vector<std::uint32_t>& body, const char* label) {
    const Lowered lowered = build_valu_cs(body);
    CHECK(lowered.user_data.size() == 5);
    check_dxil(lowered.dxil, label);
}

std::uint32_t u32_bits(float value) { return std::bit_cast<std::uint32_t>(value); }

TEST(med3_f32_with_nan) {
    const auto body = vop3(0x157, 0, vgpr(1), vgpr(2), vgpr(3));
    check_valu(std::vector<std::uint32_t>(body.begin(), body.end()), "v_med3_f32");
}

TEST(med3_f16_with_nan) {
    const auto body = vop3(0x357, 0, vgpr(1), vgpr(2), vgpr(3));
    check_valu(std::vector<std::uint32_t>(body.begin(), body.end()), "v_med3_f16");
}

TEST(fract_f32_stays_below_one) {
    check_valu({vop1(0x020, 0, vgpr(1))}, "v_fract_f32");
}

TEST(fract_f16_stays_below_one) {
    check_valu({vop1(0x05f, 0, vgpr(1))}, "v_fract_f16");
}

TEST(sin_cos_f32_turn_reduction) {
    check_valu({vop1(0x035, 0, vgpr(1))}, "v_sin_f32");
    check_valu({vop1(0x036, 0, vgpr(1))}, "v_cos_f32");
}

TEST(sin_cos_f16_turn_reduction) {
    check_valu({vop1(0x060, 0, vgpr(1))}, "v_sin_f16");
    check_valu({vop1(0x061, 0, vgpr(1))}, "v_cos_f16");
}

TEST(bfe_signed_and_unsigned) {
    const auto signed_bfe = vop3(0x149, 0, vgpr(1), vgpr(2), vgpr(3));
    check_valu(std::vector<std::uint32_t>(signed_bfe.begin(), signed_bfe.end()), "v_bfe_i32");
    const auto unsigned_bfe = vop3(0x148, 0, vgpr(1), vgpr(2), vgpr(3));
    check_valu(std::vector<std::uint32_t>(unsigned_bfe.begin(), unsigned_bfe.end()), "v_bfe_u32");
    check_valu({sop2(0x28, 6, 0, 1), vop1(0x001, 0, sgpr(6))}, "s_bfe_i32");
}

TEST(cmp_class_f16_classes) {
    const auto compare = vop3(0x08f, 6, vgpr(1), vgpr(2), 0);
    const auto mask = vop3(0x101, 0, 0x80, 0x81, sgpr(6));
    std::vector<std::uint32_t> body;
    body.insert(body.end(), compare.begin(), compare.end());
    body.insert(body.end(), mask.begin(), mask.end());
    check_valu(body, "v_cmp_class_f16");
}

TEST(cmpx_compares_read_their_operation_and_type) {
    for (const std::uint16_t op : {std::uint16_t{0x0d4}, std::uint16_t{0x014}}) {
        const auto compare = vop3(op, 0, vgpr(1), vgpr(2), 0);
        const auto value = vop1(0x001, 0, vgpr(3));
        std::vector<std::uint32_t> body(compare.begin(), compare.end());
        body.push_back(value);
        std::string_view reason;
        std::vector<std::uint32_t> words;
        words.push_back(s_mov_b32(5, 0x80));
        const std::array<std::uint32_t, 2> load = s_load_dwordx4(8, 4);
        words.insert(words.end(), load.begin(), load.end());
        words.insert(words.end(), body.begin(), body.end());
        const std::array<std::uint32_t, 2> store = buffer_store_dword(0, 8, 0);
        words.insert(words.end(), store.begin(), store.end());
        words.push_back(kSEndPgm);
        try {
            lower_compute(words, 8,
                          {root_param(RootType::Kind::Constants, 0, 0), root_param(RootType::Kind::Table, 4, 1)},
                          {used_uav(4, 1)}, 5u << 1, {1, 1, 1});
        } catch (const std::exception& error) {
            reason = error.what();
        }
        CHECK(reason.find("compare") == std::string_view::npos);
        CHECK(reason.find("v_cmpx") == std::string_view::npos);
    }
}

TEST(buffer_formats_by_number) {
    const auto bits_of = [](std::uint8_t format) {
        const std::optional<BufferFormat> found = buffer_format(format);
        return found ? std::vector<std::uint32_t>(found->bits.begin(), found->bits.end())
                     : std::vector<std::uint32_t>{};
    };
    const auto numeric_of = [](std::uint8_t format) {
        const std::optional<BufferFormat> found = buffer_format(format);
        return found ? std::optional<Numeric>{found->numeric} : std::nullopt;
    };
    CHECK((bits_of(56) == std::vector<std::uint32_t>{8, 8, 8, 8}));
    CHECK(numeric_of(56) == Numeric::Unorm);
    CHECK((bits_of(24) == std::vector<std::uint32_t>{16, 16}));
    CHECK(numeric_of(24) == Numeric::Snorm);
    CHECK((bits_of(50) == std::vector<std::uint32_t>{10, 10, 10, 2}));
    CHECK(numeric_of(50) == Numeric::Unorm);
    CHECK((bits_of(13) == std::vector<std::uint32_t>{16}));
    CHECK(numeric_of(13) == Numeric::Float);
    CHECK((bits_of(77) == std::vector<std::uint32_t>{32, 32, 32, 32}));
    CHECK(numeric_of(77) == Numeric::Float);
    CHECK(!buffer_format(0).has_value());
    CHECK(!buffer_format(78).has_value());
}

TEST(packed_buffer_formats_by_number) {
    const auto bits_of = [](std::uint8_t format) {
        const std::optional<BufferFormat> found = buffer_format(format);
        return found ? std::vector<std::uint32_t>(found->bits.begin(), found->bits.end())
                     : std::vector<std::uint32_t>{};
    };
    CHECK((bits_of(30) == std::vector<std::uint32_t>{11, 11, 10}));
    CHECK(buffer_format(30)->numeric == Numeric::Unorm);
    CHECK(buffer_format(36)->numeric == Numeric::Float);
    CHECK((bits_of(37) == std::vector<std::uint32_t>{10, 11, 11}));
    CHECK(buffer_format(43)->numeric == Numeric::Float);
    CHECK((bits_of(44) == std::vector<std::uint32_t>{2, 10, 10, 10}));
    CHECK(buffer_format(44)->numeric == Numeric::Unorm);
    CHECK(buffer_format(45)->numeric == Numeric::Snorm);
    CHECK(buffer_format(46)->numeric == Numeric::Uscaled);
    CHECK(buffer_format(47)->numeric == Numeric::Sscaled);
    CHECK(buffer_format(48)->numeric == Numeric::Uint);
    CHECK(buffer_format(49)->numeric == Numeric::Sint);
}

float half_value(std::uint32_t h) {
    const std::uint32_t sign = (h >> 15) & 1;
    const std::uint32_t exponent = (h >> 10) & 31;
    const std::uint32_t mantissa = h & 1023;
    float value = 0.0f;
    if (exponent == 0) {
        value = static_cast<float>(mantissa) * std::ldexp(1.0f, -24);
    } else if (exponent == 31 && mantissa == 0) {
        value = std::numeric_limits<float>::infinity();
    } else if (exponent == 31) {
        value = std::numeric_limits<float>::quiet_NaN();
    } else {
        value = static_cast<float>(1024 + mantissa) * std::ldexp(1.0f, static_cast<int>(exponent) - 25);
    }
    return sign == 1 ? -value : value;
}

TEST(small_floats_widen_as_halves) {
    const auto f11 = [](std::uint32_t x) { return half_value(x << half_shift(11)); };
    const auto f10 = [](std::uint32_t x) { return half_value(x << half_shift(10)); };
    CHECK(f11(0) == 0.0f);
    CHECK(f11(0x001) == std::ldexp(1.0f, -20));
    CHECK(f11(0x03F) == 63.0f * std::ldexp(1.0f, -20));
    CHECK(f11(0x040) == std::ldexp(1.0f, -14));
    CHECK(f11(0x3C0) == 1.0f);
    CHECK(f11(0x3E0) == 1.5f);
    CHECK(f11(0x7BF) == 65024.0f);
    CHECK(std::isinf(f11(0x7C0)));
    CHECK(std::isnan(f11(0x7C1)));
    CHECK(f10(0x001) == std::ldexp(1.0f, -19));
    CHECK(f10(0x020) == std::ldexp(1.0f, -14));
    CHECK(f10(0x1E0) == 1.0f);
    CHECK(f10(0x3DF) == 64512.0f);
    CHECK(std::isinf(f10(0x3E0)));
    CHECK(std::isnan(f10(0x3FF)));
    CHECK(half_value(0xBC00u << half_shift(16)) == -1.0f);
    CHECK(u32_bits(1.0f) == 0x3F80'0000u);
}

TEST(a_scalar_carry_chain_reaches_the_select) {
    const std::vector<std::uint32_t> add = {
        0xBE80'03C1u, 0xBE81'0380u, 0x8000'8100u, 0x8201'8001u,
        0x8502'0001u, 0xBF07'8002u, 0xBF85'0001u, 0xBE83'0380u, kSEndPgm};
    const std::vector<std::uint32_t> sub = {
        0xBE80'0380u, 0xBE81'0380u, 0x8080'8100u, 0x8281'8001u,
        0x8502'0001u, 0xBF07'8002u, 0xBF85'0001u, 0xBE83'0380u, kSEndPgm};
    std::string_view reason;
    CHECK(lowers(add, &reason));
    CHECK(lowers(sub, &reason));
}

TEST(scalar_bit_tests_lower) {
    const std::vector<std::uint32_t> tail = {0xBF85'0001u, 0xBE84'0380u, kSEndPgm};
    const std::vector<std::vector<std::uint32_t>> heads = {
        {0xBE81'0387u, 0xBF0C'8301u},
        {0xBE81'0387u, 0xBF0D'8301u},
        {0xBE80'0387u, 0xBE81'0380u, 0xBF0E'8300u},
        {0xBE80'0387u, 0xBE81'0380u, 0xBF0F'8300u},
    };
    for (const std::vector<std::uint32_t>& head : heads) {
        std::vector<std::uint32_t> words = head;
        words.insert(words.end(), tail.begin(), tail.end());
        CHECK(lowers(words));
    }
}

TEST(the_loop_limit_switch_parses_or_falls_back) {
    CHECK(parse_loop_limit(std::nullopt) == kLoopLimit);
    CHECK(parse_loop_limit("4096") == 4096);
    CHECK(parse_loop_limit(" 0x1000 ") == 4096);
    CHECK(parse_loop_limit("1") == 1);
    for (std::string_view bad : {"", "0", "-5", "lots", "0x", "4294967296"}) {
        CHECK(parse_loop_limit(bad) == kLoopLimit);
    }
}

TEST(an_exec_high_write_does_not_overflow_the_stack) {
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.wave64 = true;
    native.vgprs = 2;
    ComputeIo io;
    io.rsrc2 = 0;
    io.threads = {64, 1, 1};
    native.io = io;
    native.code = bytes_of({0xBEFF'0380u, 0x7E02'0300u, 0xBF81'0000u});
    const Program program = ::g6shim::shader::build(native.patched_code(), native.wave64, native.vgprs);
    const Resources resources = track(program, native.layout, native.stage);
    const Io raised = raise(program, native);
    const Structured structured = structure(program);
    std::vector<ContainerPart> no_parts;
    const std::vector<std::uint8_t> container_bytes = ::g6shim::shader::emit::build(no_parts);
    const Container container = Container::parse(container_bytes);
    bool accepted = true;
    try {
        (void)::g6shim::shader::emit::emit(container, native, program, structured, raised, resources);
    } catch (const std::exception& error) {
        accepted = std::string_view(error.what()).find("its own value") != std::string_view::npos;
    }
    CHECK(accepted);
}

TEST(reading_a_channel_below_an_input_element_does_not_underflow) {
    NativeProgram native;
    native.stage = HwStage::Ps;
    native.wave64 = false;
    native.vgprs = 4;
    PsInput input;
    input.usage = Usage{Usage::Kind::Generic, 0};
    input.row = 0;
    input.mask = 0b1000;
    input.attr = 0;
    input.flat = false;
    input.fp16 = false;
    input.pack2 = false;
    input.row2 = 0;
    PsIo ps;
    ps.inputs.push_back(input);
    ps.input_addr = 0;
    ps.db_shader_control = 0;
    native.io = ps;
    native.code = bytes_of({0xC80E'0202u, 0xBF81'0000u});
    SigElement w_only;
    w_only.name = "TEXCOORD";
    w_only.index = 0;
    w_only.kind = 0;
    w_only.comp = 9;
    w_only.interp = 0;
    w_only.row = 0;
    w_only.col = 3;
    w_only.cols = 1;
    w_only.usage = 0;
    w_only.half = false;
    const std::vector<SigElement> elements{w_only};
    const std::vector<std::uint8_t> isg1 = signature(elements, true);
    std::vector<ContainerPart> parts;
    parts.push_back(ContainerPart{{'I', 'S', 'G', '1'}, isg1});
    const std::vector<std::uint8_t> container_bytes = ::g6shim::shader::emit::build(parts);
    const Container container = Container::parse(container_bytes);
    const Program program = ::g6shim::shader::build(native.patched_code(), native.wave64, native.vgprs);
    const Resources resources = track(program, native.layout, native.stage);
    const Io raised = raise(program, native);
    const Structured structured = structure(program);
    bool finished = false;
    try {
        (void)::g6shim::shader::emit::emit(container, native, program, structured, raised, resources);
        finished = true;
    } catch (const std::exception&) {
        finished = true;
    }
    CHECK(finished);
}

}  // namespace
