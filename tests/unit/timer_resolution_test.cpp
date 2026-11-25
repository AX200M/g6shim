// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace {

constexpr std::size_t kSamples = 21;
constexpr double kConsoleStepLimitMilliseconds = 8.0;

double median_one_millisecond_sleep() {
    std::array<double, kSamples> taken{};
    for (double& sample : taken) {
        const auto started = std::chrono::steady_clock::now();
        Sleep(1);
        sample = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    std::sort(taken.begin(), taken.end());
    return taken[kSamples / 2];
}

}  // namespace

TEST(runtime_gives_the_process_one_millisecond_waits) {
    CHECK(GetModuleHandleW(L"g6shim_runtime.dll") != nullptr);
    CHECK(median_one_millisecond_sleep() < kConsoleStepLimitMilliseconds);
}
