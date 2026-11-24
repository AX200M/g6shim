// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/drives.hpp>

namespace {

using RemoveA = int(__cdecl*)(const char*);
using Wremove = int(__cdecl*)(const wchar_t*);
using StatA = int(__cdecl*)(const char*, void*);
using Wstat = int(__cdecl*)(const wchar_t*, void*);
using Wchmod = int(__cdecl*)(const wchar_t*, int);

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

extern "C" int __cdecl g6shim_remove(const char* path) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const RemoveA host_function = ucrt<RemoveA>("remove");
        return host_function != nullptr ? host_function(path) : -1;
    }
    const Wremove host_function = ucrt<Wremove>("_wremove");
    return host_function != nullptr ? host_function(host->c_str()) : -1;
}

extern "C" int __cdecl g6shim__wremove(const wchar_t* path) {
    const auto host = g6shim::drives::translate(path);
    const Wremove host_function = ucrt<Wremove>("_wremove");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path);
}

extern "C" int __cdecl g6shim__stat64i32(const char* path, void* info) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const StatA host_function = ucrt<StatA>("_stat64i32");
        return host_function != nullptr ? host_function(path, info) : -1;
    }
    const Wstat host_function = ucrt<Wstat>("_wstat64i32");
    return host_function != nullptr ? host_function(host->c_str(), info) : -1;
}

extern "C" int __cdecl g6shim__wstat64(const wchar_t* path, void* info) {
    const auto host = g6shim::drives::translate(path);
    const Wstat host_function = ucrt<Wstat>("_wstat64");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path, info);
}

extern "C" int __cdecl g6shim__wstat64i32(const wchar_t* path, void* info) {
    const auto host = g6shim::drives::translate(path);
    const Wstat host_function = ucrt<Wstat>("_wstat64i32");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path, info);
}

extern "C" int __cdecl g6shim__wchmod(const wchar_t* path, int mode) {
    const auto host = g6shim::drives::translate(path);
    const Wchmod host_function = ucrt<Wchmod>("_wchmod");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path, mode);
}
