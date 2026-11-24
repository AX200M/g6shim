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

extern "C" HANDLE WINAPI g6shim_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                            DWORD disposition, DWORD flags, HANDLE template_file) {
    return CreateFileW(host_path(g6shim::drives::translate(path), path), access, share, security, disposition,
                       flags, template_file);
}

extern "C" HANDLE WINAPI g6shim_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                            DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto p = g6shim::drives::translate(path);
    return p ? CreateFileW(p->c_str(), access, share, security, disposition, flags, template_file)
             : CreateFileA(path, access, share, security, disposition, flags, template_file);
}

extern "C" BOOL WINAPI g6shim_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    return CreateDirectoryW(host_path(g6shim::drives::translate(path), path), security);
}

extern "C" BOOL WINAPI g6shim_DeleteFileA(LPCSTR path) {
    const auto p = g6shim::drives::translate(path);
    return p ? DeleteFileW(p->c_str()) : DeleteFileA(path);
}

extern "C" BOOL WINAPI g6shim_DeleteFileW(LPCWSTR path) {
    return DeleteFileW(host_path(g6shim::drives::translate(path), path));
}

extern "C" HANDLE WINAPI g6shim_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    return FindFirstFileW(host_path(g6shim::drives::translate(path), path), data);
}

extern "C" HANDLE WINAPI g6shim_FindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
                                                 FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    return FindFirstFileExW(host_path(g6shim::drives::translate(path), path), level, data, op, filter, flags);
}

extern "C" BOOL WINAPI g6shim_GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                                  PULARGE_INTEGER free) {
    return GetDiskFreeSpaceExW(host_path(g6shim::drives::translate(path), path), available, total, free);
}

extern "C" BOOL WINAPI g6shim_GetDiskFreeSpaceW(LPCWSTR root, LPDWORD sectors_per_cluster, LPDWORD bytes_per_sector,
                                                LPDWORD free_clusters, LPDWORD clusters) {
    auto host = g6shim::drives::translate(root);
    if (host && host->size() >= g6shim::drives::kDrivePrefix && (*host)[1] == L':' &&
        ((*host)[2] == L'\\' || (*host)[2] == L'/'))
        host->resize(g6shim::drives::kDrivePrefix);
    return GetDiskFreeSpaceW(host_path(host, root), sectors_per_cluster, bytes_per_sector, free_clusters, clusters);
}

extern "C" DWORD WINAPI g6shim_GetFileAttributesA(LPCSTR path) {
    const auto p = g6shim::drives::translate(path);
    return p ? GetFileAttributesW(p->c_str()) : GetFileAttributesA(path);
}

extern "C" DWORD WINAPI g6shim_GetFileAttributesW(LPCWSTR path) {
    return GetFileAttributesW(host_path(g6shim::drives::translate(path), path));
}

extern "C" BOOL WINAPI g6shim_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    return GetFileAttributesExW(host_path(g6shim::drives::translate(path), path), level, info);
}

extern "C" BOOL WINAPI g6shim_RemoveDirectoryW(LPCWSTR path) {
    return RemoveDirectoryW(host_path(g6shim::drives::translate(path), path));
}

extern "C" BOOL WINAPI g6shim_SetFileAttributesW(LPCWSTR path, DWORD attributes) {
    return SetFileAttributesW(host_path(g6shim::drives::translate(path), path), attributes);
}
