// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <cstdint>

namespace g6shim::winrt {

namespace {

constexpr char kApplication[] = "Windows.ApplicationModel.Core.CoreApplication";
constexpr GUID kConsoleStatics = {0x9FF3799E, 0x6E83, 0x468C, {0xA5, 0xB0, 0x50, 0x1C, 0xE6, 0xBB, 0x0F, 0xA1}};
constexpr std::size_t kGetState = 0;
constexpr std::size_t kAddStateChanged = 1;
constexpr std::size_t kRemoveStateChanged = 2;
constexpr std::uint32_t kInitialState = 0;

HRESULT __stdcall get_State(void*, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = kInitialState;
    return S_OK;
}

}  // namespace

void register_core_application() {
    register_interface(kApplication, kConsoleStatics,
                       make_vtable({{kGetState, fn(&get_State)},
                                    {kAddStateChanged, event_add(kAddStateChanged)},
                                    {kRemoveStateChanged, event_remove()}}));
}

}  // namespace g6shim::winrt
