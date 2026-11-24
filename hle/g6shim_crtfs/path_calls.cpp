// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <io.h>

#include <g6shim/drives.hpp>

#include <cstdint>

namespace {

using RemoveA = int(__cdecl*)(const char*);
using Wremove = int(__cdecl*)(const wchar_t*);
using StatA = int(__cdecl*)(const char*, void*);
using Wstat = int(__cdecl*)(const wchar_t*, void*);
using Wchmod = int(__cdecl*)(const wchar_t*, int);
using Wdirectory = int(__cdecl*)(const wchar_t*);
using DirectoryA = int(__cdecl*)(const char*);

constexpr std::intptr_t kNoFind = -1;

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

template <class Narrow, class Wide>
void narrow_find_data(const Wide& wide, Narrow* out) {
    out->attrib = wide.attrib;
    out->time_create = wide.time_create;
    out->time_access = wide.time_access;
    out->time_write = wide.time_write;
    out->size = wide.size;
    if (WideCharToMultiByte(CP_ACP, 0, wide.name, -1, out->name, static_cast<int>(sizeof out->name), nullptr,
                            nullptr) == 0)
        out->name[0] = '\0';
    out->name[sizeof out->name - 1] = '\0';
}

template <class Narrow, class Wide>
std::intptr_t find_first_narrow(const char* path, Narrow* data, const char* narrow_name, const char* wide_name) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const auto host_function = ucrt<std::intptr_t(__cdecl*)(const char*, Narrow*)>(narrow_name);
        return host_function != nullptr ? host_function(path, data) : kNoFind;
    }
    const auto host_function = ucrt<std::intptr_t(__cdecl*)(const wchar_t*, Wide*)>(wide_name);
    if (host_function == nullptr) return kNoFind;
    if (data == nullptr) return host_function(host->c_str(), nullptr);
    Wide wide{};
    const std::intptr_t handle = host_function(host->c_str(), &wide);
    if (handle != kNoFind) narrow_find_data(wide, data);
    return handle;
}

template <class Wide>
std::intptr_t find_first_wide(const wchar_t* path, Wide* data, const char* name) {
    const auto host = g6shim::drives::translate(path);
    const auto host_function = ucrt<std::intptr_t(__cdecl*)(const wchar_t*, Wide*)>(name);
    if (host_function == nullptr) return kNoFind;
    return host_function(host ? host->c_str() : path, data);
}

int wide_directory_call(const wchar_t* path, const char* name) {
    const auto host = g6shim::drives::translate(path);
    const Wdirectory host_function = ucrt<Wdirectory>(name);
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path);
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

extern "C" int __cdecl g6shim__mkdir(const char* path) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const DirectoryA host_function = ucrt<DirectoryA>("_mkdir");
        return host_function != nullptr ? host_function(path) : -1;
    }
    return wide_directory_call(host->c_str(), "_wmkdir");
}

extern "C" int __cdecl g6shim__wmkdir(const wchar_t* path) { return wide_directory_call(path, "_wmkdir"); }

extern "C" int __cdecl g6shim__wrmdir(const wchar_t* path) { return wide_directory_call(path, "_wrmdir"); }

extern "C" std::intptr_t __cdecl g6shim__findfirst32(const char* path, _finddata32_t* data) {
    return find_first_narrow<_finddata32_t, _wfinddata32_t>(path, data, "_findfirst32", "_wfindfirst32");
}

extern "C" std::intptr_t __cdecl g6shim__findfirst32i64(const char* path, _finddata32i64_t* data) {
    return find_first_narrow<_finddata32i64_t, _wfinddata32i64_t>(path, data, "_findfirst32i64", "_wfindfirst32i64");
}

extern "C" std::intptr_t __cdecl g6shim__findfirst64(const char* path, __finddata64_t* data) {
    return find_first_narrow<__finddata64_t, _wfinddata64_t>(path, data, "_findfirst64", "_wfindfirst64");
}

extern "C" std::intptr_t __cdecl g6shim__findfirst64i32(const char* path, _finddata64i32_t* data) {
    return find_first_narrow<_finddata64i32_t, _wfinddata64i32_t>(path, data, "_findfirst64i32", "_wfindfirst64i32");
}

extern "C" std::intptr_t __cdecl g6shim__wfindfirst32(const wchar_t* path, _wfinddata32_t* data) {
    return find_first_wide(path, data, "_wfindfirst32");
}

extern "C" std::intptr_t __cdecl g6shim__wfindfirst32i64(const wchar_t* path, _wfinddata32i64_t* data) {
    return find_first_wide(path, data, "_wfindfirst32i64");
}

extern "C" std::intptr_t __cdecl g6shim__wfindfirst64(const wchar_t* path, _wfinddata64_t* data) {
    return find_first_wide(path, data, "_wfindfirst64");
}

extern "C" std::intptr_t __cdecl g6shim__wfindfirst64i32(const wchar_t* path, _wfinddata64i32_t* data) {
    return find_first_wide(path, data, "_wfindfirst64i32");
}
