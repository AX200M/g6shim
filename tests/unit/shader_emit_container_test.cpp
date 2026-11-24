// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "g6shim/shader/emit/container.hpp"
#include "g6shim/shader/emit/shader.hpp"

namespace {

using namespace g6shim::shader::emit;

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::string hex_string(const std::array<std::uint32_t, 4>& state) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint32_t word : state)
        for (std::size_t i = 0; i < 4; ++i) {
            const std::uint8_t byte = static_cast<std::uint8_t>(word >> (8 * i));
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0x0F]);
        }
    return out;
}

TEST(container_md5_core_matches_the_standard) {
    std::vector<std::uint8_t> block = {'a', 'b', 'c'};
    block.push_back(0x80);
    block.resize(56, 0);
    append_u64(block, 24);
    CHECK(block.size() == 64);
    std::array<std::uint32_t, 4> state = {0x6745'2301u, 0xefcd'ab89u, 0x98ba'dcfeu, 0x1032'5476u};
    md5_block(state, block.data());
    CHECK(hex_string(state) == "900150983cd24fb0d6963f7d28e17f72");
}

SigElement element(const char* name, std::uint32_t index, std::uint8_t kind, std::uint8_t comp, std::uint8_t interp,
                   std::optional<std::uint32_t> row, std::uint8_t col, std::uint8_t cols, std::uint8_t usage) {
    SigElement out;
    out.name = name;
    out.index = index;
    out.kind = kind;
    out.comp = comp;
    out.interp = interp;
    out.row = row;
    out.col = col;
    out.cols = cols;
    out.usage = usage;
    return out;
}

std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325u;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x0100'0000'01b3u;
    }
    return hash;
}

TEST(container_signature_layout_is_stable) {
    const std::vector<SigElement> inputs = {
        element("TEXCOORD", 0, 0, 9, 2, 1, 0, 4, 0),
        element("SV_Position", 0, 3, 9, 4, 0, 0, 4, 15),
    };
    const std::vector<std::uint8_t> bytes = signature(inputs, true);
    CHECK(bytes.size() == 93);
    CHECK(fnv(bytes) == 0x1b3d'0f96'70c7'3d38u);
}

TEST(container_psv_layout_is_stable) {
    const std::vector<SigElement> inputs = {
        element("TEXCOORD", 0, 0, 9, 2, 1, 0, 4, 0),
        element("SV_Position", 0, 3, 9, 4, 0, 0, 4, 15),
    };
    const std::vector<SigElement> outputs = {element("SV_Target", 0, 16, 9, 0, 0, 0, 4, 15)};
    const std::vector<Binding> bindings = {{2, 0, 0, 13}, {1, 0, 1, 14}};
    Psv description;
    description.stage = 0;
    description.flag = true;
    description.inputs = inputs;
    description.outputs = outputs;
    description.bindings = bindings;
    description.threads = {8, 4, 2};
    description.wave_size = 32;
    const std::vector<std::uint8_t> bytes = psv(description);
    CHECK(bytes.size() == 216);
    CHECK(fnv(bytes) == 0xac55'67f7'0225'a45au);
}

TEST(container_hash_skips_the_digest) {
    const std::array<std::uint8_t, 4> features = {1, 2, 3, 4};
    const std::array<std::uint8_t, 20> digest = {};
    const std::array<std::uint8_t, 3> program = {0x41, 0x42, 0x43};
    const std::vector<ContainerPart> parts = {
        ContainerPart{{'S', 'F', 'I', '0'}, features},
        ContainerPart{{'H', 'A', 'S', 'H'}, digest},
        ContainerPart{{'D', 'X', 'I', 'L'}, program},
    };
    const std::vector<std::uint8_t> bytes = build(parts);
    CHECK(bytes.size() == 95);
    CHECK(fnv(bytes) == 0x0017'011d'0e4c'b717u);
    const std::vector<std::uint8_t> expected = {0xe6, 0x47, 0x1c, 0x90, 0xa3, 0x68, 0xf6, 0x6d,
                                                0x0d, 0xd2, 0xee, 0x98, 0x53, 0x7e, 0xe0, 0xd3};
    CHECK(std::equal(expected.begin(), expected.end(), bytes.begin() + 4));
}

std::vector<std::uint8_t> constant_colour() {
    Shader shader(true);
    shader.outputs.push_back(element("SV_Target", 0, 16, 9, 0, 0, 0, 4, 15));
    const float colours[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    for (std::uint8_t channel = 0; channel < 4; ++channel) {
        const std::vector<V> args = {shader.m.i32(0), shader.m.i32(0), shader.m.i8(channel),
                                     shader.m.f32(std::bit_cast<std::uint32_t>(colours[channel]))};
        shader.op(shader.entry, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5, args);
    }
    shader.m.terminate(shader.entry, Term::ret());
    return shader.finish();
}

TEST(container_emitted_shader_is_stable) {
    const std::vector<std::uint8_t> bytes = constant_colour();
    CHECK(bytes.size() == 1594);
    const std::array<std::uint8_t, 16> digest = {0xea, 0x5f, 0x54, 0x15, 0xd8, 0xd5, 0xa0, 0xb7,
                                                 0x9e, 0x59, 0xef, 0x35, 0x3b, 0xce, 0x73, 0x3b};
    CHECK(std::equal(digest.begin(), digest.end(), bytes.begin() + 4));
}

}  // namespace
