// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace g6shim::run {

constexpr std::size_t kMaxStackFrames = 32;

std::string format_raw_address(std::uintptr_t address);
std::string format_module_address(std::uintptr_t address, std::uintptr_t module_base, std::string_view module_name);

class FrameStop {
public:
    bool keep(std::uintptr_t address);

private:
    bool has_address_ = false;
    std::uintptr_t last_address_ = 0;
};

}  // namespace g6shim::run
