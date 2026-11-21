// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/manifest.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>
#include <winstring.h>

#include <g6shim/log.hpp>

#include <cstdlib>
#include <string>

namespace g6shim::winrt {

namespace {

constexpr char kConfiguration[] = "Windows.Xbox.Services.XboxLiveConfiguration";
constexpr GUID kStatics = {0x00193376, 0x6A84, 0x4C67, {0x84, 0xA5, 0x3B, 0x8A, 0x8A, 0xAE, 0xA7, 0x12}};
constexpr char kManifestElement[] = "XboxLive";
constexpr char kTitleIdAttribute[] = "TitleId";
constexpr char kServiceConfigAttribute[] = "PrimaryServiceConfigId";
constexpr wchar_t kSandbox[] = L"RETAIL";
constexpr int kHex = 16;
constexpr std::size_t kGetTitleId = 0;
constexpr std::size_t kGetServiceConfigId = 1;
constexpr std::size_t kGetSandboxId = 2;

std::wstring widen(const std::string& s) {
    std::wstring out(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                                      static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

HRESULT to_hstring(const std::wstring& s, HSTRING* out) {
    if (!out) return E_POINTER;
    return WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), out);
}

HRESULT __stdcall get_TitleId(void*, HSTRING* out) {
    const std::string hex = manifest_attribute(manifest_text(), kManifestElement, kTitleIdAttribute);
    const unsigned long id = std::strtoul(hex.c_str(), nullptr, kHex);
    G6SHIM_INFO("configuration: title ID %lu (0x%08lX)", id, id);
    return to_hstring(std::to_wstring(id), out);
}

HRESULT __stdcall get_PrimaryServiceConfigId(void*, HSTRING* out) {
    return to_hstring(widen(manifest_attribute(manifest_text(), kManifestElement, kServiceConfigAttribute)), out);
}

HRESULT __stdcall get_SandboxId(void*, HSTRING* out) { return to_hstring(kSandbox, out); }

}  // namespace

void register_live_configuration() {
    register_interface(kConfiguration, kStatics,
                       make_vtable({{kGetTitleId, fn(&get_TitleId)},
                                    {kGetServiceConfigId, fn(&get_PrimaryServiceConfigId)},
                                    {kGetSandboxId, fn(&get_SandboxId)}}));
}

}  // namespace g6shim::winrt
