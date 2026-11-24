// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

namespace g6shim::winrt {

namespace {

constexpr char kContext[] = "Windows.Xbox.ApplicationModel.Core.CoreApplicationContext";
constexpr GUID kStatics = {0xD8D4C59F, 0xB9AC, 0x4D41, {0x84, 0x2D, 0xA0, 0x1C, 0x59, 0xB6, 0x17, 0xE7}};
constexpr std::size_t kGetCurrentUser = 0;

HRESULT __stdcall get_CurrentUser(void*, void** user) {
    if (!user) return E_POINTER;
    *user = local_user();
    return S_OK;
}

}  // namespace

void register_application_context() {
    register_interface(kContext, kStatics, make_vtable({{kGetCurrentUser, fn(&get_CurrentUser)}}));
}

}  // namespace g6shim::winrt
