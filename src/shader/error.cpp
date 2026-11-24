// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/error.hpp"

#include <cstdio>

namespace g6shim::shader {

std::string hex(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

}  // namespace g6shim::shader
