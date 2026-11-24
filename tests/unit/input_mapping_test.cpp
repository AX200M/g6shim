// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "input/mapping.hpp"

#include <windows.h>

using namespace g6shim::input;

TEST(input_parses_button_lists) {
    std::vector<std::string> unknown;
    CHECK(parse_buttons("A Menu", &unknown) == (kA | kMenu));
    CHECK(parse_buttons("  lb\trb  ", &unknown) == (kLeftShoulder | kRightShoulder));
    CHECK(parse_buttons("LT RT", &unknown) == (kLeftTriggerBit | kRightTriggerBit));
    CHECK(unknown.empty());
    CHECK(parse_buttons("A Jump", &unknown) == kA);
    CHECK(unknown.size() == 1 && unknown[0] == "Jump");
    CHECK(parse_buttons("") == 0);
}

TEST(input_parses_key_names) {
    CHECK(parse_key("Enter") == VK_RETURN);
    CHECK(parse_key("enter") == VK_RETURN);
    CHECK(parse_key("a") == 'A');
    CHECK(parse_key("7") == '7');
    CHECK(parse_key("F1") == VK_F1);
    CHECK(parse_key("f12") == VK_F12);
    CHECK(parse_key("F13") == 0);
    CHECK(parse_key("F0") == 0);
    CHECK(parse_key("Nope") == 0);
    CHECK(parse_key("") == 0);
}

TEST(input_parses_autopress_specs) {
    const auto presses = parse_autopress("6:Menu,9.5:A,,bad,3:Jump,12:Down");
    CHECK(presses.size() == 3);
    if (presses.size() == 3) {
        CHECK(presses[0].at == 6.0 && presses[0].buttons == kMenu);
        CHECK(presses[1].at == 9.5 && presses[1].buttons == kA);
        CHECK(presses[2].at == 12.0 && presses[2].buttons == kDPadDown);
    }
    CHECK(autopress_buttons(presses, 6.1) == kMenu);
    CHECK(autopress_buttons(presses, 6.0 + kPressSeconds) == 0);
    CHECK(autopress_buttons(presses, 5.9) == 0);
    std::string many;
    for (int i = 0; i < 100; ++i) many += "1:A,";
    CHECK(parse_autopress(many).size() == kMaxPresses);
}

TEST(input_hat_directions) {
    CHECK(pov_buttons(0xFFFFFFFF) == 0);
    CHECK(pov_buttons(0) == kDPadUp);
    CHECK(pov_buttons(4500) == (kDPadUp | kDPadRight));
    CHECK(pov_buttons(9000) == kDPadRight);
    CHECK(pov_buttons(18000) == kDPadDown);
    CHECK(pov_buttons(27000) == kDPadLeft);
    CHECK(pov_buttons(31500) == (kDPadUp | kDPadLeft));
}

TEST(input_without_controllers_reads_only_keys_and_presses) {
    const g6shim::input::Reading reading = g6shim::input::read_without_controllers();
    CHECK(reading.timestamp != 0);
    CHECK(reading.buttons == 0);
    CHECK(reading.left_x == 0.0f && reading.left_y == 0.0f && reading.right_x == 0.0f && reading.right_y == 0.0f);
}
