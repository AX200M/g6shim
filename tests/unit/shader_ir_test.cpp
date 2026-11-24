// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/ir.hpp"

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

Program program(const std::vector<std::uint32_t>& words, bool wave64, std::uint16_t vgprs = 256) {
    return build(bytes(words), wave64, vgprs);
}

std::optional<Value> input(const Program& program, Reg reg) {
    for (const auto& entry : program.inputs)
        if (entry.first == reg) return entry.second;
    return std::nullopt;
}

std::vector<std::uint8_t> vgprs(const std::vector<std::pair<Reg, Value>>& regs) {
    std::vector<std::uint8_t> out;
    for (const auto& entry : regs)
        if (entry.first.kind == Reg::Kind::V) out.push_back(entry.first.code);
    return out;
}

bool rejects(const std::vector<std::uint32_t>& words, bool wave64, std::uint16_t vgprs) {
    try {
        program(words, wave64, vgprs);
    } catch (const ParseError& error) {
        return error.kind() == ErrorKind::Unsupported;
    }
    return false;
}

bool fails(const std::vector<std::uint32_t>& words, bool wave64, std::uint16_t vgprs) {
    try {
        program(words, wave64, vgprs);
    } catch (const ParseError&) {
        return true;
    }
    return false;
}

TEST(ir_joins_get_phis_only_where_values_differ) {
    const std::vector<std::uint32_t> code{
        0xBF06'0100, 0xBF85'0001, 0xBE82'0380, 0x7E00'0202, 0x7E02'0200, kEndpgm,
    };
    for (bool wave64 : {false, true}) {
        const Program p = program(code, wave64);
        CHECK(p.blocks.size() == 3);
        if (p.blocks.size() != 3) continue;
        const Block& b0 = p.blocks[0];
        const Block& b1 = p.blocks[1];
        const Block& b2 = p.blocks[2];
        CHECK(b0.exit == Exit::branch(2, 1));
        CHECK(b1.exit == Exit::jump(2));
        CHECK((b2.preds == std::vector<std::size_t>{0, 1}));
        CHECK(b0.insts.size() == 2);
        CHECK(b0.insts[1].read(Reg::scc()) == b0.insts[0].write(Reg::scc()));

        CHECK(b2.phis.size() == 1);
        if (b2.phis.size() == 1) {
            const Phi& phi = b2.phis[0];
            CHECK(phi.reg == Reg::s(2));
            CHECK(phi.args.size() == 2);
            if (phi.args.size() == 2) {
                CHECK(phi.args[0] == input(p, Reg::s(2)));
                CHECK(phi.args[1] == b1.insts[0].write(Reg::s(2)));
            }
            CHECK(b2.insts.size() == 3);
            if (b2.insts.size() == 3) {
                CHECK(b2.insts[0].read(Reg::s(2)) == std::optional<Value>{phi.value});
                CHECK(b2.insts[1].read(Reg::s(0)) == input(p, Reg::s(0)));
                CHECK(b2.insts[1].read(Reg::v(1)) == input(p, Reg::v(1)));
                CHECK(b2.insts[1].read(Reg::s(kExec)) == input(p, Reg::s(kExec)));
                CHECK(b2.insts[1].read(Reg::s(kExec + 1)).has_value() == wave64);
                CHECK(b2.insts[2].name() == "s_endpgm");
                CHECK(b2.exit == Exit::end());
            }
        }
    }
}

TEST(ir_in_place_scalar_ops_read_their_destination) {
    const Program p = program({0xBE8D'1D94, 0xB78E'0010, 0xBE8F'0380, kEndpgm}, false);
    CHECK(p.blocks.size() == 1);
    if (p.blocks.size() != 1) return;
    CHECK(p.blocks[0].insts.size() == 4);
    if (p.blocks[0].insts.size() != 4) return;
    const Inst& bitset = p.blocks[0].insts[0];
    const Inst& addk = p.blocks[0].insts[1];
    const Inst& mov = p.blocks[0].insts[2];
    CHECK(bitset.read(Reg::s(13)) == input(p, Reg::s(13)));
    CHECK(addk.read(Reg::s(14)) == input(p, Reg::s(14)));
    CHECK(!mov.read(Reg::s(15)).has_value());
}

TEST(ir_loops_carry_values_through_phis) {
    const Program p = program({0xBE80'0380, 0x8000'8100, 0xBF07'8400, 0xBF85'FFFD, kEndpgm}, true);
    CHECK(p.inputs.empty());
    CHECK(p.blocks.size() == 3);
    if (p.blocks.size() != 3) return;
    const Block& b0 = p.blocks[0];
    const Block& b1 = p.blocks[1];
    const Block& b2 = p.blocks[2];
    CHECK((b1.preds == std::vector<std::size_t>{0, 1}));
    CHECK(b1.exit == Exit::branch(1, 2));
    CHECK(b1.phis.size() == 1);
    CHECK(b1.insts.size() >= 2);
    if (b1.phis.size() != 1 || b1.insts.size() < 2) return;
    const Phi& phi = b1.phis[0];
    const Inst& add = b1.insts[0];
    CHECK(phi.reg == Reg::s(0));
    CHECK(phi.args.size() == 2);
    if (phi.args.size() == 2) {
        CHECK(phi.args[0] == b0.insts[0].write(Reg::s(0)));
        CHECK(phi.args[1] == add.write(Reg::s(0)));
    }
    CHECK(add.read(Reg::s(0)) == std::optional<Value>{phi.value});
    CHECK(b1.insts[1].read(Reg::s(0)) == add.write(Reg::s(0)));
    CHECK(b2.insts[0].name() == "s_endpgm");
}

TEST(ir_drops_scheduling_and_follows_far_jumps) {
    const Program p =
        program({0xBF80'0000, 0xBE88'1F00, 0x8008'FF08, 20, 0x8209'8009, 0xBE80'2008, 0xFFFF'FFFF, kEndpgm},
              false);
    CHECK(p.blocks.size() == 2);
    if (p.blocks.size() != 2) return;
    std::vector<std::string_view> names;
    for (const Inst& inst : p.blocks[0].insts) names.push_back(inst.name());
    CHECK((names == std::vector<std::string_view>{"s_getpc_b64", "s_add_u32", "s_addc_u32", "s_setpc_b64"}));
    CHECK(p.blocks[0].exit == Exit::jump(1));
    CHECK(p.blocks[1].offset == 28);
    CHECK(p.blocks[0].insts.size() == 4);
    if (p.blocks[0].insts.size() == 4)
        CHECK(p.blocks[0].insts[2].read(Reg::scc()) == p.blocks[0].insts[1].write(Reg::scc()));
}

TEST(ir_reports_what_it_cannot_express) {
    CHECK(rejects({0xBE80'2E01, kEndpgm}, false, 256));
}

TEST(ir_relative_vgpr_access_spans_up_to_the_vgpr_count) {
    const Program p = program({0x7E04'8501, 0x7E00'8704, kEndpgm}, false, 6);
    CHECK(p.blocks.size() == 1);
    if (p.blocks.size() != 1) return;
    CHECK(p.blocks[0].insts.size() == 3);
    if (p.blocks[0].insts.size() != 3) return;
    const Inst& movreld = p.blocks[0].insts[0];
    const Inst& movrels = p.blocks[0].insts[1];
    CHECK(vgprs(movreld.writes) == std::vector<std::uint8_t>({2, 3, 4, 5}));
    CHECK(vgprs(movreld.reads) == std::vector<std::uint8_t>({1, 2, 3, 4, 5}));
    CHECK(movreld.read(Reg::s(kM0)).has_value());
    CHECK(vgprs(movrels.writes) == std::vector<std::uint8_t>({0}));
    CHECK(vgprs(movrels.reads) == std::vector<std::uint8_t>({4, 5, 0}));
    CHECK(movrels.read(Reg::v(4)) == movreld.write(Reg::v(4)));
    CHECK(fails({0x7E04'8501, 0x7E00'8704, kEndpgm}, false, 4));
}

}  // namespace
