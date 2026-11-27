// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "timer_resolution.hpp"

#include <windows.h>
#include <timeapi.h>

namespace g6shim::timer {

namespace {

constexpr UINT kConsoleTimerMilliseconds = 1;

void keep_resolution_in_background() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
}

}  // namespace

void use_console_resolution() {
    keep_resolution_in_background();
    timeBeginPeriod(kConsoleTimerMilliseconds);
}

}  // namespace g6shim::timer
