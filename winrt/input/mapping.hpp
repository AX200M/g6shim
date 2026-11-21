// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::input {

enum Button : std::uint32_t {
    kMenu = 0x1,
    kView = 0x2,
    kA = 0x4,
    kB = 0x8,
    kX = 0x10,
    kY = 0x20,
    kDPadUp = 0x40,
    kDPadDown = 0x80,
    kDPadLeft = 0x100,
    kDPadRight = 0x200,
    kLeftShoulder = 0x400,
    kRightShoulder = 0x800,
    kLeftThumbstick = 0x1000,
    kRightThumbstick = 0x2000,
};

inline constexpr std::uint32_t kLeftTriggerBit = 0x10000;
inline constexpr std::uint32_t kRightTriggerBit = 0x20000;

struct Reading {
    std::uint64_t timestamp;
    std::uint32_t buttons;
    float left_trigger, right_trigger;
    float left_x, left_y;
    float right_x, right_y;
};
static_assert(sizeof(Reading) == 40);

struct Press {
    double at;
    std::uint32_t buttons;
};

inline constexpr double kPressSeconds = 0.2;
inline constexpr std::size_t kMaxPresses = 64;
inline constexpr std::uint32_t kPovCentered = 0xFFFF;

std::uint32_t parse_buttons(std::string_view text, std::vector<std::string>* unknown = nullptr);
int parse_key(std::string_view name);
std::vector<Press> parse_autopress(std::string_view spec);
std::uint32_t autopress_buttons(const std::vector<Press>& presses, double seconds);
std::uint32_t pov_buttons(std::uint32_t pov);

Reading read();

}  // namespace g6shim::input
