// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <cstdint>

namespace g6shim::winrt {

namespace {

constexpr char kMouse[] = "Windows.Devices.Input.MouseDevice";
constexpr GUID kStatics = {0xF5A82CE3, 0x4A2D, 0x4004, {0xB3, 0x86, 0xC7, 0x4D, 0xC6, 0x9B, 0xE9, 0x79}};
constexpr GUID kIMouseDevice = {0x88EDF458, 0xF2C8, 0x49F4, {0xBE, 0x1F, 0xC2, 0x56, 0xB3, 0x88, 0xBC, 0x11}};
constexpr char kCapabilities[] = "Windows.Devices.Input.MouseCapabilities";
constexpr GUID kIMouseCapabilities = {0x56CB2884, 0x282D, 0x49F7, {0xA1, 0xBC, 0x45, 0x6E, 0xCC, 0x2E, 0x5D, 0xE8}};
constexpr std::size_t kGetForCurrentView = 0;
constexpr std::size_t kAddMouseMoved = 0;
constexpr std::size_t kRemoveMouseMoved = 1;
constexpr std::size_t kCapabilityCount = 5;
constexpr std::int32_t kNoCapability = 0;

HRESULT __stdcall GetForCurrentView(void*, void** out) {
    if (!out) return E_POINTER;
    static void* const device = make_probe(kMouse, kIMouseDevice);
    *out = add_ref(device);
    return S_OK;
}

HRESULT __stdcall get_Capability(void*, std::int32_t* value) {
    if (!value) return E_POINTER;
    *value = kNoCapability;
    return S_OK;
}

}  // namespace

void register_mouse() {
    register_interface(kMouse, kStatics, make_vtable({{kGetForCurrentView, fn(&GetForCurrentView)}}));
    register_interface(kMouse, kIMouseDevice,
                       make_vtable({{kAddMouseMoved, event_add(kAddMouseMoved)}, {kRemoveMouseMoved, event_remove()}}));
    const void* capability = fn(&get_Capability);
    static_assert(kCapabilityCount == 5);
    register_interface(kCapabilities, kIMouseCapabilities,
                       make_vtable({{0, capability}, {1, capability}, {2, capability}, {3, capability}, {4, capability}}));
}

}  // namespace g6shim::winrt
