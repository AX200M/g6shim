// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

namespace g6shim::winrt {

namespace {

constexpr char kManager[] = "Windows.Xbox.Management.Deployment.DownloadableContentPackageManager";
constexpr GUID kIManager = {0x971FEAAF, 0x98F8, 0x4038, {0xB8, 0xC1, 0x3A, 0x5F, 0x1A, 0x04, 0x3B, 0x97}};
constexpr std::size_t kFindPackages = 0;
constexpr std::size_t kAddInstallCompleted = 1;
constexpr std::size_t kRemoveInstallCompleted = 2;

HRESULT __stdcall FindPackages(void*, int, void** packages) {
    if (!packages) return E_POINTER;
    *packages = make_empty_vector_view();
    return S_OK;
}

}  // namespace

void register_content_packages() {
    register_interface(kManager, kIManager,
                       make_vtable({{kFindPackages, fn(&FindPackages)},
                                    {kAddInstallCompleted, event_add(kAddInstallCompleted)},
                                    {kRemoveInstallCompleted, event_remove()}}));
}

}  // namespace g6shim::winrt
