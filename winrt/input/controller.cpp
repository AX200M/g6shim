// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"
#include "mapping.hpp"

#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>
#include <winstring.h>

#include <cstdint>
#include <iterator>

namespace g6shim::winrt {

namespace {

constexpr char kController[] = "Windows.Xbox.Input.Controller";
constexpr char kGamepad[] = "Windows.Xbox.Input.Gamepad";
constexpr wchar_t kGamepadType[] = L"Windows.Xbox.Input.Gamepad";
constexpr GUID kControllerStatics = {0x7400624F, 0x144B, 0x47FC, {0x9F, 0x3B, 0x1D, 0xCA, 0xFB, 0x48, 0x28, 0x73}};
constexpr GUID kIController = {0x9776FF56, 0x9BFD, 0x4581, {0xAD, 0x45, 0xB6, 0x45, 0xBB, 0xA5, 0x26, 0xD6}};
constexpr GUID kGamepadStatics = {0x8C4C1BCD, 0xFDAB, 0x4BF8, {0xBD, 0xE9, 0xAB, 0xB2, 0xB1, 0xD6, 0xFD, 0xD3}};
constexpr std::uint64_t kControllerId = 1;

namespace statics {
constexpr std::size_t kList = 0;
constexpr std::size_t kAddFirst = 1;
constexpr std::size_t kRemoveFirst = 2;
constexpr std::size_t kAddSecond = 3;
constexpr std::size_t kRemoveSecond = 4;
constexpr std::size_t kAddThird = 5;
constexpr std::size_t kRemoveThird = 6;
}  // namespace statics

namespace controller {
constexpr std::size_t kId = 0;
constexpr std::size_t kType = 1;
}  // namespace controller

namespace gamepad {
constexpr std::size_t kSetVibration = 0;
constexpr std::size_t kGetCurrentReading = 1;
constexpr std::size_t kGetRawCurrentReading = 2;
}  // namespace gamepad

HRESULT __stdcall Controller_get_Id(void*, std::uint64_t* id) {
    if (!id) return E_POINTER;
    *id = kControllerId;
    return S_OK;
}

HRESULT __stdcall Controller_get_Type(void*, HSTRING* type) {
    if (!type) return E_POINTER;
    return WindowsCreateString(kGamepadType, static_cast<UINT32>(std::size(kGamepadType) - 1), type);
}

HRESULT __stdcall Gamepad_SetVibration(void*, const float* vibration) { return vibration ? S_OK : E_POINTER; }

HRESULT __stdcall Gamepad_GetRawCurrentReading(void*, input::Reading* reading) {
    if (!reading) return E_POINTER;
    *reading = input::read();
    return S_OK;
}

HRESULT __stdcall get_Controllers(void*, void** controllers) {
    if (!controllers) return E_POINTER;
    static void* one = make_probe(kController, kNoIid);
    *controllers = make_vector_view({one});
    return S_OK;
}

HRESULT __stdcall get_Gamepads(void*, void** gamepads) {
    if (!gamepads) return E_POINTER;
    static void* one = make_probe(kGamepad, kNoIid);
    *gamepads = make_vector_view({one});
    return S_OK;
}

}  // namespace

void register_controller() {
    register_interface(kController, kControllerStatics,
                       make_vtable({{statics::kList, fn(&get_Controllers)},
                                    {statics::kAddFirst, event_add(statics::kAddFirst)},
                                    {statics::kRemoveFirst, event_remove()},
                                    {statics::kAddSecond, event_add(statics::kAddSecond)},
                                    {statics::kRemoveSecond, event_remove()},
                                    {statics::kAddThird, event_add(statics::kAddThird)},
                                    {statics::kRemoveThird, event_remove()}}));
    register_interface(kController, kNoIid, make_vtable({{controller::kId, fn(&Controller_get_Id)}}));
    register_interface(kController, kIController,
                       make_vtable({{controller::kId, fn(&Controller_get_Id)}, {controller::kType, fn(&Controller_get_Type)}}));
    register_interface(kGamepad, kNoIid,
                       make_vtable({{gamepad::kSetVibration, fn(&Gamepad_SetVibration)},
                                    {gamepad::kGetCurrentReading, gamepad_get_current_reading()},
                                    {gamepad::kGetRawCurrentReading, fn(&Gamepad_GetRawCurrentReading)}}));
    register_interface(kGamepad, kIController,
                       make_vtable({{controller::kId, fn(&Controller_get_Id)}, {controller::kType, fn(&Controller_get_Type)}}));
    register_interface(kGamepad, kGamepadStatics,
                       make_vtable({{statics::kList, fn(&get_Gamepads)},
                                    {statics::kAddFirst, event_add(statics::kAddFirst)},
                                    {statics::kRemoveFirst, event_remove()},
                                    {statics::kAddSecond, event_add(statics::kAddSecond)},
                                    {statics::kRemoveSecond, event_remove()}}));
}

}  // namespace g6shim::winrt
