// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <fileapi.h>

#include <g6shim/drives.hpp>

namespace {

LPCWSTR host_path(const std::optional<std::wstring>& path, LPCWSTR original) {
    return path ? path->c_str() : original;
}

}

extern "C" BOOL WINAPI g6shim_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    return MoveFileExW(host_path(g6shim::drives::translate(from), from),
                       host_path(g6shim::drives::translate(to), to), flags);
}
