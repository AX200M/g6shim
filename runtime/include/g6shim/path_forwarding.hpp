// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <fileapi.h>

#include <g6shim/drives.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace g6shim::path_forwarding {

inline LPCWSTR host_or_original(const std::optional<std::wstring>& host, LPCWSTR original) {
    return host ? host->c_str() : original;
}

template <std::size_t N>
void narrow_name(const wchar_t (&wide)[N], char* out, std::size_t out_size) {
    const int written = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, static_cast<int>(out_size), nullptr, nullptr);
    if (written == 0 && out_size > 0) out[0] = '\0';
    if (out_size > 0) out[out_size - 1] = '\0';
}

inline void narrow_find_data(const WIN32_FIND_DATAW& wide, WIN32_FIND_DATAA* out) {
    out->dwFileAttributes = wide.dwFileAttributes;
    out->ftCreationTime = wide.ftCreationTime;
    out->ftLastAccessTime = wide.ftLastAccessTime;
    out->ftLastWriteTime = wide.ftLastWriteTime;
    out->nFileSizeHigh = wide.nFileSizeHigh;
    out->nFileSizeLow = wide.nFileSizeLow;
    out->dwReserved0 = wide.dwReserved0;
    out->dwReserved1 = wide.dwReserved1;
    narrow_name(wide.cFileName, out->cFileName, sizeof out->cFileName);
    narrow_name(wide.cAlternateFileName, out->cAlternateFileName, sizeof out->cAlternateFileName);
}

inline BOOL create_directory_a(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    const auto host = drives::translate(path);
    return host ? CreateDirectoryW(host->c_str(), security) : CreateDirectoryA(path, security);
}

inline HANDLE find_first_file_ex_a(LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
                                   LPVOID filter, DWORD flags) {
    const auto host = drives::translate(path);
    if (!host || data == nullptr) return FindFirstFileExA(path, level, data, op, filter, flags);
    WIN32_FIND_DATAW wide{};
    const HANDLE handle = FindFirstFileExW(host->c_str(), level, &wide, op, filter, flags);
    if (handle != INVALID_HANDLE_VALUE) narrow_find_data(wide, static_cast<WIN32_FIND_DATAA*>(data));
    return handle;
}

inline std::optional<std::wstring> drive_rooted(LPCWSTR path) {
    if (!drives::translate(path) || path[0] == L'\\') return std::nullopt;
    if (path[2] == L'\\' || path[2] == L'/') return std::nullopt;
    std::wstring rooted(path, 2);
    rooted += L'\\';
    rooted += path + 2;
    return rooted;
}

inline DWORD full_path_name_w(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR* file_part) {
    return GetFullPathNameW(host_or_original(drive_rooted(path), path), size, buffer, file_part);
}

inline BOOL copy_file_w(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
    return CopyFileW(host_or_original(drives::translate(from), from), host_or_original(drives::translate(to), to),
                     fail_if_exists);
}

inline HRESULT copy_file2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    return CopyFile2(host_or_original(drives::translate(from), from), host_or_original(drives::translate(to), to),
                     parameters);
}

}  // namespace g6shim::path_forwarding
