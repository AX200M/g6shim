// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <cstdint>
#include <tuple>
#include <vector>

#include "g6shim/shader/emit/wave.hpp"
#include "g6shim/shader/ir.hpp"

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

}  // namespace
