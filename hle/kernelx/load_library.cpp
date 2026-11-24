// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/log.hpp>

#include <cwchar>
#include <string>

namespace {

constexpr DWORD kSearchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                               LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
constexpr wchar_t kDefaultExtension[] = L".dll";

std::wstring in_game_folder(LPCWSTR name) {
    if (std::wcspbrk(name, L"\\/:")) return {};
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring path(exe, n);
    path.erase(path.find_last_of(L"\\/") + 1);
    path += name;
    if (!std::wcschr(name, L'.')) path += kDefaultExtension;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return path;
}

}  // namespace

extern "C" HMODULE WINAPI g6shim_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (name && (flags & LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const std::wstring path = in_game_folder(name);
        if (!path.empty()) {
            HMODULE module = LoadLibraryExW(path.c_str(), file, flags & ~kSearchFlags);
            G6SHIM_INFO("LoadLibraryExW(%ls, System32 only) -> the game folder's -> %p", name,
                        static_cast<void*>(module));
            return module;
        }
    }
    return LoadLibraryExW(name, file, flags);
}
