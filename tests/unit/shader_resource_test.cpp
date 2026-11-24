// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"
#include "g6shim/shader/structure.hpp"

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

std::optional<Structured> lanes_of(const Program& p) {
    try {
        return structure(p);
    } catch (const ParseError&) {
        return std::nullopt;
    }
}

Resources track_words(const std::vector<std::uint32_t>& words, const std::vector<RootParam>& layout,
                      HwStage stage) {
    const Program p = build(bytes(words), false, 256);
    const std::optional<Structured> lanes = lanes_of(p);
    return track(p, layout, stage, lanes ? &*lanes : nullptr);
}

RootParam param(RootType::Kind kind, std::uint8_t user_data, std::uint32_t sgprs) {
    RootParam entry;
    entry.root = RootType{kind, 0};
    entry.user_data = user_data;
    entry.sgprs = sgprs;
    return entry;
}

Address at(std::size_t parameter, std::uint32_t offset) {
    Address address;
    address.param = parameter;
    address.offset = offset;
    return address;
}

TEST(resource_table_descriptors_are_loaded_from_their_range) {
    const Resources r = track_words(
        {0xBE82'0309, 0xBE89'0380, 0xBE83'0380, 0xF408'0204, 0xFA00'0000, 0xF40C'0001, 0xFA00'0000,
         0xF080'0F08, 0x0040'0002, kEndpgm},
        {param(RootType::Kind::Table, 8, 1), param(RootType::Kind::Table, 9, 1)}, HwStage::Ps);
    const std::vector<std::pair<Site, Address>> expected{{Site{0, 3}, at(0, 0)}, {Site{0, 4}, at(1, 0)}};
    CHECK(r.scalar == expected);
    std::vector<std::pair<Role, Source>> roles;
    for (const Descriptor& descriptor : r.descriptors) roles.emplace_back(descriptor.role, descriptor.source);
    const std::vector<std::pair<Role, Source>> wanted{
        {Role::Resource, Source::loaded(Site{0, 4}, 0)},
        {Role::Sampler, Source::loaded(Site{0, 3}, 0)},
    };
    CHECK(roles == wanted);
}

TEST(resource_root_descriptors_are_built_in_code) {
    const Resources r = track_words(
        {0x8801'FF0B, 0x0010'0000, 0xBE80'030A, 0xBE82'03C1, 0xBE83'03FF, 0x1101'4FAC, 0xF430'0000,
         0xFA00'0000, kEndpgm},
        {param(RootType::Kind::Descriptor, 10, 2)}, HwStage::Vs);
    CHECK(r.descriptors.size() == 1);
    if (r.descriptors.size() == 1)
        CHECK(r.descriptors[0].source ==
              Source::built(at(0, 0), 0x10'0000, std::array<std::uint32_t, 2>{0xFFFF'FFFF, 0x1101'4FAC}));
    std::vector<std::pair<std::size_t, std::uint32_t>> params;
    for (const auto& entry : r.params) params.emplace_back(std::get<1>(entry), std::get<2>(entry));
    CHECK(params == (std::vector<std::pair<std::size_t, std::uint32_t>>{{0, 0}, {0, 1}}));
}

TEST(resource_heap_indices_from_root_constants_in_a_merged_stage) {
    const Resources r = track_words(
        {0x8F08'850E, 0xBE93'0380, 0xF408'0209, 0x1000'0000, 0xE000'2000, 0x8002'0400, kEndpgm},
        {param(RootType::Kind::Constants, 6, 1), param(RootType::Kind::ResourceHeap, 10, 1)}, HwStage::Ms);
    Value index{};
    for (const auto& entry : r.params)
        if (std::get<1>(entry) == 0) index = std::get<0>(entry);
    CHECK(r.scalar.size() == 1);
    if (r.scalar.size() != 1) return;
    CHECK(r.scalar[0].second.dynamic.has_value());
    if (r.scalar[0].second.dynamic) CHECK(*r.scalar[0].second.dynamic != index);
    CHECK(r.scalar[0].second.param == 1);
    CHECK(r.descriptors.size() == 1);
    if (r.descriptors.size() == 1)
        CHECK(r.descriptors[0].source == Source::loaded(Site{0, 2}, 0));
}

TEST(resource_spilled_sgprs_are_followed_through_vgpr_lanes) {
    const Resources r = track_words(
        {0xD761'0000, 0x0001'0609, 0xD760'0002, 0x0001'0700, 0xBE83'0380, 0xF40C'0001, 0xFA00'0020,
         kEndpgm},
        {param(RootType::Kind::Table, 9, 1)}, HwStage::Ps);
    const std::vector<std::pair<Site, Address>> expected{{Site{0, 3}, at(0, 0x20)}};
    CHECK(r.scalar == expected);
}

TEST(resource_descriptors_joined_by_control_flow_are_traced_per_edge) {
    const Resources r = track_words(
        {0xBE82'0309, 0xBE83'0380, 0xBF06'8001, 0xBF85'0003, 0xF408'0001, 0xFA00'0000, 0xBF82'0002,
         0xF408'0001, 0xFA00'0010, 0xF420'0100, 0xFA00'0000, kEndpgm},
        {param(RootType::Kind::Table, 9, 1)}, HwStage::Ps);
    CHECK(r.descriptors.size() == 1);
    if (r.descriptors.size() != 1) return;
    const Source& source = r.descriptors[0].source;
    CHECK(source.kind == Source::Kind::Phi);
    std::vector<std::uint32_t> offsets;
    for (const Source& arg : source.args) {
        CHECK(arg.kind == Source::Kind::Loaded);
        for (const auto& entry : r.scalar)
            if (entry.first == arg.load) offsets.push_back(entry.second.offset);
    }
    CHECK(offsets.size() == 2);
    bool zero = false;
    bool sixteen = false;
    for (std::uint32_t offset : offsets) {
        if (offset == 0) zero = true;
        if (offset == 0x10) sixteen = true;
    }
    CHECK(zero && sixteen);
}

TEST(resource_descriptors_selected_by_scc_are_traced_per_outcome) {
    const Resources r = track_words(
        {0xBE8C'0309, 0xBE8D'0380, 0xF408'0006, 0xFA00'0000, 0xF408'0106, 0xFA00'0010, 0xBF06'800E,
         0x8588'0400, 0x858A'0602, 0xF420'0404, 0xFA00'0000, kEndpgm},
        {param(RootType::Kind::Table, 9, 1)}, HwStage::Ps);
    CHECK(r.descriptors.size() == 1);
    if (r.descriptors.size() != 1) return;
    const Source& source = r.descriptors[0].source;
    CHECK(source.kind == Source::Kind::Select);
    CHECK(source.args == (std::vector<Source>{Source::loaded(Site{0, 2}, 0), Source::loaded(Site{0, 3}, 0)}));
    CHECK(r.scalar == (std::vector<std::pair<Site, Address>>{{Site{0, 2}, at(0, 0)}, {Site{0, 3}, at(0, 0x10)}}));
}

TEST(resource_a_selection_that_mixes_two_descriptors_fails) {
    const Program p = build(bytes({0xBE8C'0309, 0xBE8D'0380, 0xF408'0006, 0xFA00'0000, 0xF408'0106,
                                   0xFA00'0010, 0xBF06'800E, 0x8588'0400, 0x858A'0202, 0xF420'0404,
                                   0xFA00'0000, kEndpgm}),
                            false, 256);
    const std::optional<Structured> lanes = lanes_of(p);
    bool rejected = false;
    try {
        (void)track(p, {param(RootType::Kind::Table, 9, 1)}, HwStage::Ps, lanes ? &*lanes : nullptr);
    } catch (const ParseError& error) {
        rejected = error.kind() == ErrorKind::Unsupported &&
                   std::string(error.what()).find("a descriptor changed after loading") !=
                       std::string::npos;
    }
    CHECK(rejected);
}

TEST(resource_untraceable_addresses_fail_with_a_reason) {
    const Program p = build(bytes({0xF408'0001, 0xFA00'0000, kEndpgm}), false, 256);
    const std::optional<Structured> lanes = lanes_of(p);
    bool rejected = false;
    try {
        track(p, {param(RootType::Kind::Table, 9, 1)}, HwStage::Ps, lanes ? &*lanes : nullptr);
    } catch (const ParseError& error) {
        rejected = error.kind() == ErrorKind::Unsupported &&
                   std::string(error.what()).find("an address in an SGPR that is no root parameter") !=
                       std::string::npos;
    }
    CHECK(rejected);
}

}  // namespace
