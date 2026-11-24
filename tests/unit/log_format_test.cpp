// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "log_format.hpp"

#include <clocale>
#include <cstdint>
#include <string>

namespace {

std::string elapsed(std::int64_t microseconds) {
    return g6shim::detail::format_elapsed(std::chrono::microseconds(microseconds));
}

void check_values() {
    CHECK(elapsed(0) == "    0.000");
    CHECK(elapsed(3'664'000) == "    3.664");
    CHECK(elapsed(59'999'000) == "   59.999");
    CHECK(elapsed(12'345'678'000) == "12345.678");
    CHECK(elapsed(999'500) == "    1.000");
    CHECK(elapsed(1'000'499) == "    1.000");
    CHECK(elapsed(1'000'500) == "    1.001");
}

}  // namespace

TEST(log_timestamps_are_locale_independent) {
    check_values();
    CHECK(std::setlocale(LC_ALL, "de-DE") != nullptr);
    check_values();
    CHECK(std::setlocale(LC_ALL, "C") != nullptr);
}
