// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace g6shim::cpu {

inline constexpr std::size_t kCacheLineBytes = 64;
inline constexpr std::size_t kMaxInstructionLength = 15;
inline constexpr std::size_t kInstructionReadBytes = 32;
inline constexpr std::uint32_t kComparedFlags = 0x8d5;

struct ClwbLoop {
    std::uint8_t base;
    std::uint8_t limit;
    std::size_t length;
};

std::optional<ClwbLoop> clwb_loop(const std::uint8_t* code, std::size_t size);
std::optional<std::size_t> clwb_length(const std::array<std::uint8_t, kMaxInstructionLength>& code);
std::uint32_t compare_flags(std::uint64_t a, std::uint64_t b);
bool emulate_amd_only(EXCEPTION_POINTERS* exception);
void install_amd_emulation();

}  // namespace g6shim::cpu
