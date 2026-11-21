// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/async.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <g6shim/log.hpp>

namespace g6shim::winrt {

namespace {

constexpr char kSystemUi[] = "Windows.Xbox.UI.SystemUI";
constexpr char kResult[] = "Windows.Xbox.UI.AccountPickerResult";
constexpr char kResultObject2[] = "Windows.Xbox.UI.AccountPickerResult#2";
constexpr GUID kStatics = {0x1A0DD80E, 0x3F9D, 0x4CFA, {0x8E, 0x50, 0x0E, 0x2E, 0xBD, 0x1A, 0xA7, 0x2F}};
constexpr std::size_t kShowAccountPicker = 4;
constexpr std::size_t kResultUser = 0;
constexpr std::size_t kResultObject2Method = 2;

HRESULT __stdcall ShowAccountPickerAsync(void*, void* controller, unsigned options, void** operation) {
    if (!operation) return E_POINTER;
    G6SHIM_INFO("SystemUI: account picker (controller %p, options %u) -> the local player", controller, options);
    void* result = make_probe(kResult, kNoIid);
    *operation = make_completed_async(result);
    release(result);
    return S_OK;
}

HRESULT __stdcall Result_get_User(void*, void** out) {
    if (!out) return E_POINTER;
    *out = local_user();
    return S_OK;
}

HRESULT __stdcall Result_get_Object2(void*, void** out) {
    if (!out) return E_POINTER;
    *out = make_probe(kResultObject2, kNoIid);
    return S_OK;
}

}  // namespace

void register_account_picker() {
    register_interface(kResult, kNoIid,
                       make_vtable({{kResultUser, fn(&Result_get_User)}, {kResultObject2Method, fn(&Result_get_Object2)}}));
    register_interface(kSystemUi, kStatics, make_vtable({{kShowAccountPicker, fn(&ShowAccountPickerAsync)}}));
}

}  // namespace g6shim::winrt
