// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/log.hpp>

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

constexpr char kModuleName[] = "g6shim_libloader2";
constexpr std::size_t kRuntimeBaseLength = 12;
constexpr std::size_t kExtensionLength = 4;
constexpr wchar_t kRuntimeBase[] = L"xgameruntime";
constexpr char kRuntimeBaseNarrow[] = "xgameruntime";
constexpr wchar_t kRuntimeFile[] = L"g6shim_xgameruntime.dll";
constexpr wchar_t kWideExtension[] = L".dll";
constexpr char kNarrowExtension[] = ".dll";

static_assert(sizeof(kRuntimeBase) / sizeof(wchar_t) == kRuntimeBaseLength + 1);
static_assert(sizeof(kRuntimeBaseNarrow) == kRuntimeBaseLength + 1);

const wchar_t* wide_base_name(const wchar_t* name) {
    const wchar_t* base = name;
    for (const wchar_t* at = name; *at != L'\0'; ++at)
        if (*at == L'\\' || *at == L'/') base = at + 1;
    return base;
}

const char* narrow_base_name(const char* name) {
    const char* base = name;
    for (const char* at = name; *at != '\0'; ++at)
        if (*at == '\\' || *at == '/') base = at + 1;
    return base;
}

bool requests_runtime(const wchar_t* name) {
    if (name == nullptr) return false;
    const wchar_t* base = wide_base_name(name);
    std::size_t length = std::wcslen(base);
    if (length >= kExtensionLength && _wcsicmp(base + length - kExtensionLength, kWideExtension) == 0)
        length -= kExtensionLength;
    return length == kRuntimeBaseLength && _wcsnicmp(base, kRuntimeBase, length) == 0;
}

bool requests_runtime(const char* name) {
    if (name == nullptr) return false;
    const char* base = narrow_base_name(name);
    std::size_t length = std::strlen(base);
    if (length >= kExtensionLength && _stricmp(base + length - kExtensionLength, kNarrowExtension) == 0)
        length -= kExtensionLength;
    return length == kRuntimeBaseLength && _strnicmp(base, kRuntimeBaseNarrow, length) == 0;
}

std::wstring executable_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    path.erase(path.find_last_of(L"\\/") + 1);
    return path;
}

HMODULE load_runtime() {
    const std::wstring folder = executable_folder();
    if (folder.empty()) return nullptr;
    const std::wstring path = folder + kRuntimeFile;
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, 0);
    G6SHIM_INFO("%s: xgameruntime.dll -> %ls -> %p", kModuleName, path.c_str(), static_cast<void*>(module));
    return module;
}

}  // namespace

extern "C" HMODULE WINAPI g6shim_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (requests_runtime(name)) {
        if (const HMODULE module = load_runtime()) return module;
    }
    return LoadLibraryExW(name, file, flags);
}

extern "C" HMODULE WINAPI g6shim_LoadLibraryW(LPCWSTR name) {
    if (requests_runtime(name)) {
        if (const HMODULE module = load_runtime()) return module;
    }
    return LoadLibraryW(name);
}

extern "C" HMODULE WINAPI g6shim_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (requests_runtime(name)) {
        if (const HMODULE module = load_runtime()) return module;
    }
    return LoadLibraryExA(name, file, flags);
}

extern "C" HMODULE WINAPI g6shim_LoadLibraryA(LPCSTR name) {
    if (requests_runtime(name)) {
        if (const HMODULE module = load_runtime()) return module;
    }
    return LoadLibraryA(name);
}
