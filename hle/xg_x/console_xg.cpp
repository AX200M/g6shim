// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console_xg.hpp"

#include <g6shim/log.hpp>

namespace g6shim::xg {

namespace {

std::wstring own_module_path() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&own_module_path), &self))
        return {};
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(path, n) : std::wstring();
}

HMODULE load_console_library() {
    const std::wstring path = console_library_path(own_module_path());
    if (path.empty()) return nullptr;
    HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (library)
        G6SHIM_INFO("XG: using the console library %ls", path.c_str());
    else
        G6SHIM_INFO("XG: no console library at %ls, using g6shim's layouts", path.c_str());
    return library;
}

}  // namespace

std::wstring console_library_path(const std::wstring& module_path) {
    const std::size_t slash = module_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return module_path.substr(0, slash + 1) + kConsoleLibraryName;
}

HMODULE console_library() {
    static const HMODULE library = load_console_library();
    return library;
}

FARPROC console_export(const char* name) {
    const HMODULE library = console_library();
    return library ? GetProcAddress(library, name) : nullptr;
}

}  // namespace g6shim::xg
