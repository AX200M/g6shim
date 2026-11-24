// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/cpu_emulation.hpp>

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>

namespace {

constexpr std::size_t kCodeOffset = 0x800;

alignas(4096) std::uint8_t g_code[4096];

std::uint8_t* code_at(const std::uint8_t* code, std::size_t length) {
    std::memset(g_code, 0xcc, sizeof g_code);
    std::memcpy(g_code + kCodeOffset, code, length);
    return g_code + kCodeOffset;
}

std::optional<std::uint64_t> emulate_as(std::uint32_t exception, const std::uint8_t* code, std::size_t length,
                                        std::uint64_t rax, std::array<M128A, 16>& xmm) {
    std::uint8_t* at = code_at(code, length);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = exception;
    CONTEXT context{};
    context.Rip = reinterpret_cast<DWORD64>(at);
    context.Rax = rax;
    std::memcpy(context.FltSave.XmmRegisters, xmm.data(), sizeof(M128A) * xmm.size());
    EXCEPTION_POINTERS pointers{&record, &context};

    const bool emulated = g6shim::cpu::emulate_amd_only(&pointers);
    std::memcpy(xmm.data(), context.FltSave.XmmRegisters, sizeof(M128A) * xmm.size());
    if (!emulated) return std::nullopt;
    return static_cast<std::uint64_t>(context.Rip) - reinterpret_cast<std::uintptr_t>(at);
}

std::optional<std::uint64_t> emulate_as(std::uint32_t exception, std::initializer_list<std::uint8_t> code,
                                        std::uint64_t rax, std::array<M128A, 16>& xmm) {
    return emulate_as(exception, code.begin(), code.size(), rax, xmm);
}

std::optional<std::uint64_t> emulate(std::initializer_list<std::uint8_t> code, std::uint64_t rax,
                                     std::array<M128A, 16>& xmm) {
    return emulate_as(EXCEPTION_ILLEGAL_INSTRUCTION, code, rax, xmm);
}

M128A reg(std::uint64_t low, std::uint64_t high) {
    return M128A{low, static_cast<LONGLONG>(high)};
}

std::uint64_t low(const std::array<M128A, 16>& xmm, std::size_t index) { return xmm[index].Low; }

std::array<std::uint8_t, g6shim::cpu::kMaxInstructionLength> pad(std::initializer_list<std::uint8_t> bytes) {
    std::array<std::uint8_t, g6shim::cpu::kMaxInstructionLength> code{};
    std::size_t at = 0;
    for (std::uint8_t byte : bytes) code[at++] = byte;
    return code;
}

}  // namespace

TEST(cpu_clwb_loop_is_recognised) {
    using namespace g6shim::cpu;

    const std::uint8_t rcx_loop[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1,
                                     0x40, 0x48, 0x39, 0xc1, 0x76, 0xf3};
    const auto rcx = clwb_loop(rcx_loop, sizeof rcx_loop);
    CHECK(rcx.has_value());
    if (rcx) {
        CHECK(rcx->base == 1);
        CHECK(rcx->limit == 0);
        CHECK(rcx->length == 13);
    }

    const std::uint8_t r9_loop[] = {0x66, 0x41, 0x0f, 0xae, 0x31, 0x49, 0x83,
                                    0xc1, 0x40, 0x49, 0x39, 0xc1, 0x76, 0xf2};
    const auto r9 = clwb_loop(r9_loop, sizeof r9_loop);
    CHECK(r9.has_value());
    if (r9) {
        CHECK(r9->base == 9);
        CHECK(r9->limit == 0);
        CHECK(r9->length == 14);
    }

    const std::uint8_t other_stride[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1,
                                         0x20, 0x48, 0x39, 0xc1, 0x76, 0xf3};
    CHECK(!clwb_loop(other_stride, sizeof other_stride).has_value());

    const std::uint8_t other_jump[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1,
                                       0x40, 0x48, 0x39, 0xc1, 0x76, 0xf0};
    CHECK(!clwb_loop(other_jump, sizeof other_jump).has_value());

    const std::uint8_t cut_off[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1, 0x40};
    CHECK(!clwb_loop(cut_off, sizeof cut_off).has_value());

    const std::uint8_t same_register[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1,
                                          0x40, 0x48, 0x39, 0xc9, 0x76, 0xf3};
    CHECK(!clwb_loop(same_register, sizeof same_register).has_value());

    CHECK((compare_flags(0x1040, 0x1000) & 0x41) == 0);
    CHECK((compare_flags(0x1000, 0x1000) & 0x40) == 0x40);
}

TEST(cpu_emulated_clwb_loop_is_retired) {
    using namespace g6shim::cpu;

    const std::uint8_t loop[] = {0x66, 0x0f, 0xae, 0x31, 0x48, 0x83, 0xc1,
                                 0x40, 0x48, 0x39, 0xc1, 0x76, 0xf3};
    std::uint8_t* at = code_at(loop, sizeof loop);

    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    CONTEXT context{};
    context.Rip = reinterpret_cast<DWORD64>(at);
    context.Rax = 0x1000;
    context.Rcx = 0x0f00;
    context.EFlags = 0xffffffff;
    EXCEPTION_POINTERS pointers{&record, &context};

    CHECK(emulate_amd_only(&pointers));
    CHECK(context.Rip == reinterpret_cast<DWORD64>(at) + 13);
    CHECK(context.Rcx == 0x1040);
    CHECK(context.EFlags == (0xffffffffu & ~kComparedFlags));
}

TEST(cpu_clwb_lengths) {
    using namespace g6shim::cpu;

    CHECK(clwb_length(pad({0x66, 0x41, 0x0f, 0xae, 0x31})) == std::size_t{5});
    CHECK(clwb_length(pad({0x66, 0x0f, 0xae, 0x30})) == std::size_t{4});
    CHECK(clwb_length(pad({0x66, 0x0f, 0xae, 0x70, 0x40})) == std::size_t{5});
    CHECK(clwb_length(pad({0x66, 0x0f, 0xae, 0xb0, 0, 1, 0, 0})) == std::size_t{8});
    CHECK(clwb_length(pad({0x66, 0x0f, 0xae, 0x34, 0x24})) == std::size_t{5});
    CHECK(clwb_length(pad({0x66, 0x0f, 0xae, 0x35, 0, 0, 0, 0})) == std::size_t{8});
    CHECK(!clwb_length(pad({0x66, 0x0f, 0xae, 0x38})).has_value());
    CHECK(!clwb_length(pad({0x0f, 0xae, 0x30})).has_value());
    CHECK(!clwb_length(pad({0x66, 0x0f, 0xae, 0xf0})).has_value());
}

TEST(cpu_clzero_zeroes_its_line) {
    struct alignas(64) Line {
        std::uint8_t bytes[128];
    };
    Line line{};
    std::memset(line.bytes, 0xff, sizeof line.bytes);
    std::array<M128A, 16> xmm{};
    const std::uint64_t at = reinterpret_cast<std::uintptr_t>(line.bytes + 5);

    CHECK(emulate({0x0f, 0x01, 0xfc}, at, xmm) == std::uint64_t{3});

    bool zeroed = true;
    bool untouched = true;
    for (std::size_t i = 0; i < 64; ++i) zeroed = zeroed && line.bytes[i] == 0;
    for (std::size_t i = 64; i < 128; ++i) untouched = untouched && line.bytes[i] == 0xff;
    CHECK(zeroed);
    CHECK(untouched);
}

TEST(cpu_emulated_clwb_is_skipped) {
    std::array<M128A, 16> xmm{};
    CHECK(emulate({0x66, 0x0f, 0xae, 0x30}, 0, xmm) == std::uint64_t{4});
}

TEST(cpu_monitorx_and_mwaitx_are_skipped) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(1, 2);

    CHECK(emulate({0x0f, 0x01, 0xfa}, 0, xmm) == std::uint64_t{3});
    CHECK(emulate({0x0f, 0x01, 0xfb}, 0, xmm) == std::uint64_t{3});

    bool unchanged = true;
    for (const M128A& value : xmm) unchanged = unchanged && value.Low == 1 && value.High == 2;
    CHECK(unchanged);
}

TEST(cpu_insertq_and_extrq_with_immediates) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);
    xmm[0] = reg(0x1111222233334444, 0x5555);
    xmm[1] = reg(0xabcd, 0x6666);

    const std::uint8_t insertq[] = {0xf2, 0x0f, 0x78, 0xc1, 0x10, 0x10};
    CHECK(emulate_as(EXCEPTION_ILLEGAL_INSTRUCTION, insertq, sizeof insertq, 0, xmm) == std::uint64_t{6});
    CHECK(low(xmm, 0) == 0x11112222abcd4444);
    CHECK(low(xmm, 1) == 0xabcd);
    CHECK(xmm[1].High == 0x6666);

    const std::uint8_t extrq[] = {0x66, 0x0f, 0x78, 0xc0, 0x08, 0x14};
    CHECK(emulate_as(EXCEPTION_ILLEGAL_INSTRUCTION, extrq, sizeof extrq, 0, xmm) == std::uint64_t{6});
    CHECK(low(xmm, 0) == 0xbc);
}

TEST(cpu_insertq_and_extrq_with_fields_in_a_register) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);
    xmm[0] = reg(0x1111222233334444, 0);
    xmm[1] = reg(0xabcd, 0x1010);

    CHECK(emulate({0xf2, 0x0f, 0x79, 0xc1}, 0, xmm) == std::uint64_t{4});
    CHECK(low(xmm, 0) == 0x11112222abcd4444);

    xmm[1] = reg(0x1408, 0);
    CHECK(emulate({0x66, 0x0f, 0x79, 0xc1}, 0, xmm) == std::uint64_t{4});
    CHECK(low(xmm, 0) == 0xbc);
}

TEST(cpu_sse4a_reaches_xmm8_to_xmm15) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);
    xmm[8] = reg(0x1111222233334444, 0);
    xmm[9] = reg(0xabcd, 0);

    const std::uint8_t insertq[] = {0xf2, 0x45, 0x0f, 0x78, 0xc1, 0x10, 0x10};
    CHECK(emulate_as(EXCEPTION_ILLEGAL_INSTRUCTION, insertq, sizeof insertq, 0, xmm) == std::uint64_t{7});
    CHECK(low(xmm, 8) == 0x11112222abcd4444);
    CHECK(low(xmm, 0) == 0);
    CHECK(low(xmm, 1) == 0);

    const std::uint8_t extrq[] = {0x66, 0x41, 0x0f, 0x78, 0xc1, 0x08, 0x04};
    CHECK(emulate_as(EXCEPTION_ILLEGAL_INSTRUCTION, extrq, sizeof extrq, 0, xmm) == std::uint64_t{7});
    CHECK(low(xmm, 9) == 0xbc);
    CHECK(low(xmm, 1) == 0);
}

TEST(cpu_sse4a_length_zero_is_64_bits) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);
    xmm[0] = reg(0x1122334455667788, 0);

    CHECK(emulate({0x66, 0x0f, 0x78, 0xc0, 0x00, 0x00}, 0, xmm) == std::uint64_t{6});
    CHECK(low(xmm, 0) == 0x1122334455667788);

    xmm[1] = reg(0xdeadbeefcafef00d, 0);
    CHECK(emulate({0xf2, 0x0f, 0x78, 0xc1, 0x00, 0x00}, 0, xmm) == std::uint64_t{6});
    CHECK(low(xmm, 0) == 0xdeadbeefcafef00d);
}

TEST(cpu_other_instructions_and_exceptions_are_not_emulated) {
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);

    CHECK(!emulate({0x0f, 0x0b}, 0, xmm).has_value());
    CHECK(!emulate({0x66, 0x0f, 0x79, 0x00}, 0, xmm).has_value());
    CHECK(!emulate({0x66, 0x0f, 0x78, 0xc8, 0x08, 0x14}, 0, xmm).has_value());
    CHECK(!emulate_as(EXCEPTION_ACCESS_VIOLATION, {0x0f, 0x01, 0xfc}, 0, xmm).has_value());
}

TEST(cpu_clzero_and_sse4a_sequence) {
    struct alignas(64) Line {
        std::uint8_t bytes[128];
    };
    Line line{};
    std::memset(line.bytes, 0xff, sizeof line.bytes);
    std::array<M128A, 16> xmm{};
    for (M128A& value : xmm) value = reg(0, 0);
    xmm[0] = reg(0x1111222233334444, 0);
    xmm[1] = reg(0xabcd, 0);

    const std::uint64_t at = reinterpret_cast<std::uintptr_t>(line.bytes + 5);
    CHECK(emulate({0x0f, 0x01, 0xfc}, at, xmm) == std::uint64_t{3});
    CHECK(emulate({0xf2, 0x0f, 0x78, 0xc1, 0x10, 0x10}, 0, xmm) == std::uint64_t{6});
    CHECK(emulate({0x66, 0x0f, 0x78, 0xc0, 0x08, 0x14}, 0, xmm) == std::uint64_t{6});
    CHECK(low(xmm, 0) == 0xbc);
    CHECK(emulate({0x0f, 0x01, 0xfa}, at, xmm) == std::uint64_t{3});
    CHECK(emulate({0x0f, 0x01, 0xfb}, at, xmm) == std::uint64_t{3});
}
