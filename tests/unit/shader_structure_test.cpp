// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/ir.hpp"
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

Program program(const std::vector<std::uint32_t>& words, bool wave64) {
    return build(bytes(words), wave64, 256);
}

std::size_t guarded(const Structured& structured) {
    std::size_t count = 0;
    for (const auto& row : structured.guards)
        for (const auto& guard : row)
            if (guard) ++count;
    return count;
}

bool is_lane(Bool value, const Structured& structured) {
    const auto node = structured.bools.node(value);
    return node && std::get<0>(*node).kind == Atom::Kind::Lane;
}

bool is_uniform(Bool value, const Structured& structured) {
    const auto support = structured.bools.support(value);
    return support.size() == 1 && support[0].kind == Atom::Kind::Uniform;
}

TEST(structure_divergent_if_becomes_a_lane_branch) {
    const Program p = program({0x7D84'0080, 0xBE80'3C6A, 0xBF88'0001, 0x7E02'0280, 0x887E'007E, 0x7E04'0280,
                             kEndpgm},
                            false);
    const Structured s = structure(p);
    CHECK(s.exits.size() == 3);
    if (s.exits.size() != 3) return;
    const LaneExit& exit = s.exits[0];
    CHECK(exit.kind == LaneExit::Kind::Branch);
    CHECK(exit.target == 2);
    CHECK(exit.not_taken == 1);
    const auto cmp = p.blocks[0].insts[0].write(Reg::s(kVcc));
    CHECK(cmp.has_value());
    const auto node = s.bools.node(exit.when);
    CHECK(node.has_value());
    if (node && cmp) {
        CHECK(std::get<0>(*node) == Atom::lane(*cmp));
        CHECK(std::get<1>(*node) == kTrue);
        CHECK(std::get<2>(*node) == kFalse);
    }
    CHECK(guarded(s) == 0);
}

TEST(structure_exec_narrowed_without_a_branch_leaves_a_guard) {
    const Program p = program({0x7DA4'0080, 0x7E02'0280, kEndpgm}, true);
    const Structured s = structure(p);
    CHECK(s.guards.size() == 1);
    if (s.guards.size() != 1) return;
    CHECK(s.guards[0].size() == 3);
    if (s.guards[0].size() != 3) return;
    CHECK(!s.guards[0][0].has_value());
    CHECK(s.guards[0][1].has_value());
    if (s.guards[0][1]) {
        const auto support = s.bools.support(*s.guards[0][1]);
        CHECK(support.size() == 1);
        if (support.size() == 1) CHECK(support[0].kind == Atom::Kind::Lane);
    }
}

TEST(structure_divergent_loop_exits_per_lane) {
    const Program p = program({0xBE82'0480, 0x4A00'0081, 0x7D84'0080, 0x8882'026A, 0x8AFE'027E, 0xBF89'FFFB,
                             0x88FE'027E, 0x7E02'0280, kEndpgm},
                            true);
    const Structured s = structure(p);
    CHECK(guarded(s) == 0);
    CHECK(s.exits.size() > 1);
    if (s.exits.size() <= 1) return;
    const LaneExit& exit = s.exits[1];
    CHECK(exit.kind == LaneExit::Kind::Branch);
    CHECK(exit.target == 1);
    CHECK(exit.not_taken == 2);
    CHECK(is_lane(exit.when, s));
}

TEST(structure_scalar_branches_stay_uniform) {
    const Program p = program({0xBF06'0100, 0xBF85'0001, 0x7E02'0280, kEndpgm}, true);
    const Structured s = structure(p);
    CHECK(s.exits.size() == 3);
    if (s.exits.size() != 3) return;
    CHECK(s.exits[0].kind == LaneExit::Kind::Branch);
    CHECK(is_uniform(s.exits[0].when, s));
    CHECK(guarded(s) == 0);
}

TEST(structure_irreducible_loops_are_split) {
    const Program p = program({0xBF06'0100, 0xBF85'0001, 0x8002'8102, 0xBF84'FFFE, kEndpgm}, false);
    CHECK(p.blocks.size() == 5);
    if (p.blocks.size() != 5) return;
    const Block& copy = p.blocks[4];
    CHECK(copy.offset == 8);
    CHECK(copy.exit == Exit::jump(2));
    CHECK(p.blocks[0].exit.successors() == std::vector<std::size_t>({2, 4}));
    CHECK(p.blocks[1].preds == std::vector<std::size_t>({2}));
    const Structured s = structure(p);
    CHECK(s.exits.size() == p.blocks.size());
}

}  // namespace
