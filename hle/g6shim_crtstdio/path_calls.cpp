// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/drives.hpp>

#include <cstdio>
#include <string>

namespace {

using FopenA = std::FILE*(__cdecl*)(const char*, const char*);
using FopenS = int(__cdecl*)(std::FILE**, const char*, const char*);
using Wfopen = std::FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using WfopenS = int(__cdecl*)(std::FILE**, const wchar_t*, const wchar_t*);
using Wfsopen = std::FILE*(__cdecl*)(const wchar_t*, const wchar_t*, int);
using OpenA = int(__cdecl*)(const char*, int, int);
using Wopen = int(__cdecl*)(const wchar_t*, int, int);

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

std::wstring widen_mode(const char* mode) {
    std::wstring out;
    if (mode != nullptr)
        for (const char* c = mode; *c != '\0'; ++c)
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    out.push_back(L'\0');
    return out;
}

}  // namespace

extern "C" std::FILE* __cdecl g6shim__wfopen(const wchar_t* path, const wchar_t* mode) {
    const auto host = g6shim::drives::translate(path);
    const Wfopen host_function = ucrt<Wfopen>("_wfopen");
    if (host_function == nullptr) return nullptr;
    return host_function(host ? host->c_str() : path, mode);
}

extern "C" int __cdecl g6shim__wfopen_s(std::FILE** out, const wchar_t* path, const wchar_t* mode) {
    const auto host = g6shim::drives::translate(path);
    const WfopenS host_function = ucrt<WfopenS>("_wfopen_s");
    if (host_function == nullptr) return -1;
    return host_function(out, host ? host->c_str() : path, mode);
}

extern "C" std::FILE* __cdecl g6shim__wfsopen(const wchar_t* path, const wchar_t* mode, int share) {
    const auto host = g6shim::drives::translate(path);
    const Wfsopen host_function = ucrt<Wfsopen>("_wfsopen");
    if (host_function == nullptr) return nullptr;
    return host_function(host ? host->c_str() : path, mode, share);
}

extern "C" std::FILE* __cdecl g6shim_fopen(const char* path, const char* mode) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const FopenA host_function = ucrt<FopenA>("fopen");
        return host_function != nullptr ? host_function(path, mode) : nullptr;
    }
    const Wfopen host_function = ucrt<Wfopen>("_wfopen");
    if (host_function == nullptr) return nullptr;
    return host_function(host->c_str(), widen_mode(mode).c_str());
}

extern "C" int __cdecl g6shim_fopen_s(std::FILE** out, const char* path, const char* mode) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const FopenS host_function = ucrt<FopenS>("fopen_s");
        return host_function != nullptr ? host_function(out, path, mode) : -1;
    }
    const WfopenS host_function = ucrt<WfopenS>("_wfopen_s");
    if (host_function == nullptr) return -1;
    return host_function(out, host->c_str(), widen_mode(mode).c_str());
}

extern "C" int __cdecl g6shim__open(const char* path, int flags, int mode) {
    const auto host = g6shim::drives::translate(path);
    if (!host) {
        const OpenA host_function = ucrt<OpenA>("_open");
        return host_function != nullptr ? host_function(path, flags, mode) : -1;
    }
    const Wopen host_function = ucrt<Wopen>("_wopen");
    if (host_function == nullptr) return -1;
    return host_function(host->c_str(), flags, mode);
}

extern "C" int __cdecl g6shim__wopen(const wchar_t* path, int flags, int mode) {
    const auto host = g6shim::drives::translate(path);
    const Wopen host_function = ucrt<Wopen>("_wopen");
    if (host_function == nullptr) return -1;
    return host_function(host ? host->c_str() : path, flags, mode);
}
