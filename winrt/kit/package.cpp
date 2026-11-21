// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/manifest.hpp>
#include <g6shim/winrt/package.hpp>

#include <windows.h>
#include <activation.h>
#include <winstring.h>

#include <g6shim/log.hpp>

#include <map>
#include <mutex>
#include <string>

namespace g6shim::winrt {

namespace {

constexpr char kFactoryExport[] = "DllGetActivationFactory";

std::wstring widen(const std::string& s) {
    std::wstring out(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                                      static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

using DllGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, IActivationFactory**);

DllGetActivationFactoryFn factory_function(const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, DllGetActivationFactoryFn> known;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = known.find(path);
    if (it != known.end()) return it->second;
    DllGetActivationFactoryFn fn = nullptr;
    const std::wstring full = game_folder() + widen(path);
    if (HMODULE dll = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        fn = reinterpret_cast<DllGetActivationFactoryFn>(GetProcAddress(dll, kFactoryExport));
        G6SHIM_INFO("package: loaded %s for its WinRT classes%s", path.c_str(), fn ? "" : ", but it has no factory");
    } else {
        G6SHIM_WARN("package: %s doesn't load (error %lu); its classes become probes. A missing dependency, such as "
                    "the Visual C++ 2012 runtime (x64), is the usual cause",
                    path.c_str(), GetLastError());
    }
    known[path] = fn;
    return fn;
}

}  // namespace

long activate_from_package(const char* class_name, const GUID& iid, void** factory, bool& listed) {
    static const std::map<std::string, std::string> servers = parse_in_process_servers(manifest_text());
    const auto it = servers.find(class_name);
    listed = it != servers.end();
    if (!listed || !factory) return REGDB_E_CLASSNOTREG;
    const DllGetActivationFactoryFn get = factory_function(it->second);
    if (!get) return REGDB_E_CLASSNOTREG;
    const std::wstring name = widen(class_name);
    HSTRING hs = nullptr;
    if (FAILED(WindowsCreateString(name.c_str(), static_cast<UINT32>(name.size()), &hs))) return E_OUTOFMEMORY;
    IActivationFactory* f = nullptr;
    HRESULT hr = get(hs, &f);
    WindowsDeleteString(hs);
    if (FAILED(hr) || !f) return FAILED(hr) ? hr : E_FAIL;
    hr = f->QueryInterface(iid, factory);
    f->Release();
    return hr;
}

}  // namespace g6shim::winrt
