// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/drives.hpp>

namespace {

using Wutime = int(__cdecl*)(const wchar_t*, void*);

HMODULE ucrtbase() {
    static const HMODULE module = [] {
        HMODULE found = GetModuleHandleW(L"ucrtbase.dll");
        if (found == nullptr) found = LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return found;
    }();
    return module;
}

template <class Function>
Function ucrt(const char* name) {
    const HMODULE module = ucrtbase();
    return module != nullptr ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

}  // namespace

extern "C" int __cdecl g6shim__wutime64(const wchar_t* path, void* times) {
    const auto host = g6shim::drives::translate(path);
    const Wutime host_function = ucrt<Wutime>("_wutime64");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path, times);
}
