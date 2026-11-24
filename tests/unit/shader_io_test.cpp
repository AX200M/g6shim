// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/io.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"

namespace {

using namespace g6shim::shader;

constexpr std::uint32_t kEndpgm = 0xBF81'0000;

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

Program program(const std::vector<std::uint32_t>& words) { return build(bytes(words), false, 256); }

std::optional<Value> input(const Program& p, std::uint8_t reg) {
    for (const auto& entry : p.inputs)
        if (entry.first == Reg::v(reg)) return entry.second;
    return std::nullopt;
}

PsInput texcoord() {
    PsInput value;
    value.usage = Usage{Usage::Kind::Generic, 0};
    value.row = 1;
    value.mask = 3;
    value.attr = 0;
    value.row2 = 1;
    return value;
}

NativeProgram pixel(std::vector<PsInput> inputs, std::uint32_t input_addr) {
    NativeProgram native;
    native.stage = HwStage::Ps;
    native.wave64 = false;
    native.vgprs = 256;
    PsIo io;
    io.inputs = std::move(inputs);
    io.input_addr = input_addr;
    native.io = std::move(io);
    return native;
}

NativeProgram mesh_native() {
    NativeProgram native;
    native.stage = HwStage::Ms;
    native.wave64 = true;
    native.vgprs = 256;
    MeshIo io;
    io.threads = 128;
    io.max_vertices = 128;
    io.max_primitives = 128;
    io.topology = 2;
    native.io = std::move(io);
    return native;
}

bool has_value(const Io& io, std::optional<Value> value) {
    if (!value) return false;
    for (const auto& entry : io.inputs)
        if (entry.first == *value) return true;
    return false;
}

bool has_input(const Io& io, std::optional<Value> value, const Input& expected) {
    if (!value) return false;
    for (const auto& entry : io.inputs)
        if (entry.first == *value && entry.second == expected) return true;
    return false;
}

bool raises(const Program& p, const NativeProgram& native) {
    try {
        raise(p, native);
    } catch (const ParseError&) {
        return true;
    }
    return false;
}

TEST(io_interpolation_pairs_and_color_exports) {
    const Program p = program({0xC808'0000, 0xC809'0001, 0xF800'180F, 0x0302'0102, kEndpgm});
    const Io io = raise(p, pixel({texcoord()}, 2));
    const Input center0 = Input::of_barycentric(Barycentric::PerspectiveCenter, 0);
    const Input center1 = Input::of_barycentric(Barycentric::PerspectiveCenter, 1);
    CHECK(has_input(io, input(p, 0), center0));
    CHECK(has_input(io, input(p, 1), center1));
    CHECK(!has_value(io, input(p, 3)));
    const Attribute attribute{Usage{Usage::Kind::Generic, 0}, 1, 0, false};
    const std::vector<std::pair<Site, Interp>> wanted{
        {Site{0, 1}, Interp::interpolated(attribute, Barycentric::PerspectiveCenter, {Site{0, 0}})},
    };
    CHECK(io.interps == wanted);
    CHECK(io.exports.size() == 1);
    if (io.exports.size() != 1) return;
    const Export& e = io.exports[0];
    CHECK(e.target == Target::color(0));
    CHECK(e.done);
    CHECK(!e.compressed);
    CHECK(e.values[0] == p.blocks[0].insts[1].write(Reg::v(2)));
}

TEST(io_unpaired_or_unknown_interpolation_fails) {
    CHECK(raises(program({0xC809'0001, kEndpgm}), pixel({texcoord()}, 2)));
    CHECK(raises(program({0xC808'0400, kEndpgm}), pixel({texcoord()}, 2)));
}

TEST(io_mesh_inputs_and_primitive_export) {
    const Program p = build(bytes({0x7E04'0505, 0xF800'0941, 0x0000'0009, kEndpgm}), true, 256);
    const Io io = raise(p, mesh_native());
    bool mesh_group = false;
    for (const auto& entry : io.inputs)
        if (entry.second == Input::simple(Input::Kind::MeshGroup)) mesh_group = true;
    CHECK(mesh_group);
    CHECK(io.exports.size() == 1);
    if (io.exports.size() == 1) CHECK(io.exports[0].target == Target::primitive());
    const Program per_lane = build(bytes({0x7E02'0305, kEndpgm}), true, 256);
    CHECK(raises(per_lane, mesh_native()));
}

TEST(io_pixel_vgprs_follow_the_input_address) {
    const auto v = pixel_input_vgprs((1u << 5) | (1u << 8) | (1u << 9));
    const std::vector<std::pair<std::uint8_t, Input>> wanted{
        {std::uint8_t{0}, Input::of_barycentric(Barycentric::LinearCenter, 0)},
        {std::uint8_t{1}, Input::of_barycentric(Barycentric::LinearCenter, 1)},
        {std::uint8_t{2}, Input::of_position(0)},
        {std::uint8_t{3}, Input::of_position(1)},
    };
    CHECK(v == wanted);
}

}  // namespace
