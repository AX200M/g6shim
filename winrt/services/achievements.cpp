// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

namespace g6shim::winrt {

namespace {

constexpr char kNotifier[] = "Windows.Xbox.Achievements.AchievementNotifier";
constexpr GUID kStatics = {0x82CA180B, 0x30E2, 0x4EA0, {0xA6, 0x2E, 0xB6, 0x16, 0x97, 0xA2, 0xF3, 0x34}};
constexpr std::size_t kGetNotifier = 0;

HRESULT __stdcall get_Notifier(void*, void** notifier) {
    if (!notifier) return E_POINTER;
    *notifier = make_probe(kNotifier, kNoIid);
    return S_OK;
}

}  // namespace

void register_achievements() {
    register_interface(kNotifier, kStatics, make_vtable({{kGetNotifier, fn(&get_Notifier)}}));
}

}  // namespace g6shim::winrt
