// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

namespace g6shim::winrt {

namespace {

constexpr char kTemplate[] = "Windows.Xbox.Networking.SecureDeviceAssociationTemplate";
constexpr GUID kTemplateStatics = {0x46745A18, 0x697A, 0x47E1, {0xBB, 0x62, 0xCC, 0x71, 0xDE, 0x0F, 0xDD, 0x8C}};
constexpr std::size_t kGetTemplates = 1;

HRESULT __stdcall get_Templates(void*, void** templates) {
    if (!templates) return E_POINTER;
    *templates = make_empty_vector_view();
    return S_OK;
}

}  // namespace

void register_networking() {
    register_interface(kTemplate, kTemplateStatics, make_vtable({{kGetTemplates, fn(&get_Templates)}}));
}

}  // namespace g6shim::winrt
