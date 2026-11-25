// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frames.hpp"

#include <cstdio>

namespace g6shim::run {

std::string format_raw_address(std::uintptr_t address) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(address));
    return buf;
}

std::string format_module_address(std::uintptr_t address, std::uintptr_t module_base, std::string_view module_name) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(address - module_base));
    return std::string(module_name.data(), module_name.size()) + buf;
}

bool FrameStop::keep(std::uintptr_t address) {
    if (address == 0) return false;
    if (has_address_ && address == last_address_) return false;
    has_address_ = true;
    last_address_ = address;
    return true;
}

}  // namespace g6shim::run
