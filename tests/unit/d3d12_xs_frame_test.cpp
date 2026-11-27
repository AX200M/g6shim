// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/frame.hpp"

#include <chrono>
#include <cstdint>

using namespace g6shim::d3d12_xs;

TEST(d3d12_xs_waits_end_on_ticks_and_count_frames) {
    CHECK(frame::set_interval(nullptr, nullptr, 2'000, 1, 0) == S_OK);
    std::uint64_t first = 0;
    CHECK(frame::wait(nullptr, 0, 1'000, nullptr, 0, &first) == S_OK);
    const auto before = std::chrono::steady_clock::now();
    std::uint64_t second = 0;
    CHECK(frame::wait(nullptr, 0, 1'000, nullptr, 0, &second) == S_OK);
    CHECK(second > first);
    CHECK(std::chrono::steady_clock::now() - before <= std::chrono::milliseconds(50));
    CHECK(frame::wait(nullptr, 0, 0, nullptr, 0, &second) == S_FALSE);
    std::uint64_t early = 0;
    CHECK(frame::wait(nullptr, 0, 0, nullptr, frame::kTokenOnTimeout, &early) == S_FALSE);
    std::uint64_t next = 0;
    CHECK(frame::wait(nullptr, 0, 1'000, nullptr, 0, &next) == S_OK);
    CHECK(early == second + 1 && next == early + 1);
    CHECK(frame::last_tick(0) >= 2);

    constexpr std::uint32_t kFlip = 2;
    CHECK(frame::set_interval(nullptr, nullptr, 5'000, 3, 0) == S_OK);
    std::uint64_t token = 0;
    std::uint64_t flips[4] = {};
    for (std::uint64_t& flip : flips) {
        frame::wait(nullptr, 0, 0, nullptr, frame::kTokenOnTimeout, &token);
        CHECK(frame::wait(nullptr, kFlip, 0xFFFF'FFFF, nullptr, 0, &token) == S_OK);
        flip = frame::last_tick(kFlip);
    }
    for (std::size_t i = 1; i < 4; ++i) CHECK(flips[i] == flips[i - 1] + 1);
    CHECK(frame::next_tick_number(kFlip) > flips[3]);
}
