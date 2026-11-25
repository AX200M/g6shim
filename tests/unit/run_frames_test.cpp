// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "frames.hpp"

#include <cstdint>

namespace {

void check_formatting() {
    CHECK(g6shim::run::format_raw_address(0) == "0x0");
    CHECK(g6shim::run::format_raw_address(0x7ff6'1234'5678ULL) == "0x7ff612345678");
    CHECK(g6shim::run::format_module_address(0x7ff6'1234'5000ULL, 0x7ff6'1234'0000ULL, "game.exe") ==
          "game.exe+0x5000");
    CHECK(g6shim::run::format_module_address(0x1000, 0x1000, "a.dll") == "a.dll+0x0");
    CHECK(g6shim::run::format_module_address(0xffff'ffff'ffff'ffffULL, 0xffff'ffff'ffff'0000ULL, "b.dll") ==
          "b.dll+0xffff");
}

void check_stop_rules() {
    g6shim::run::FrameStop stop;
    CHECK(stop.keep(0x1000));
    CHECK(stop.keep(0x2000));
    CHECK(!stop.keep(0));
    CHECK(stop.keep(0x3000));
    CHECK(!stop.keep(0x3000));
    CHECK(stop.keep(0x4000));
    CHECK(!stop.keep(0x4000));
    CHECK(stop.keep(0x2000));
}

}  // namespace

TEST(run_frames_format_addresses) {
    check_formatting();
}

TEST(run_frames_stop_on_zero_and_repeated_address) {
    check_stop_rules();
}
