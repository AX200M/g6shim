// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <fileapi.h>

#include <g6shim/drives.hpp>
#include <g6shim/log.hpp>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace {

constexpr std::size_t kMaxLogged = 500;
constexpr USHORT kBacktraceFrames = 24;
constexpr wchar_t kCrt[] = L"ucrtbase.dll";

std::wstring widen(LPCSTR path) {
    if (!path) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring out(n > 0 ? static_cast<std::size_t>(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, path, -1, out.data(), n);
    if (!out.empty()) out.pop_back();
    return out;
}

void log_failure(LPCWSTR path, DWORD error) {
    static std::mutex mutex;
    static std::set<std::wstring> seen;
    const std::wstring key = path ? path : L"(null)";
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.size() >= kMaxLogged || !seen.insert(key).second) return;
    }
    char utf8[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    void* frames[kBacktraceFrames] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, kBacktraceFrames, frames, nullptr);
    std::uintptr_t caller = 0;
    for (USHORT i = 0; i < n && !caller; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (a >= exe && a < exe + nt->OptionalHeader.SizeOfImage) caller = a - exe;
    }
    G6SHIM_INFO("CreateFile \"%s\" failed: error %lu (from the exe at +0x%llx)", utf8, error,
                static_cast<unsigned long long>(caller));
}

LPCWSTR host_path(const std::optional<std::wstring>& path, LPCWSTR original) {
    return path ? path->c_str() : original;
}

}  // namespace

extern "C" HANDLE WINAPI g6shim_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                            DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto p = g6shim::drives::translate(path);
    const HANDLE h =
        CreateFileW(host_path(p, path), access, share, security, disposition, flags, template_file);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        log_failure(host_path(p, path), error);
        SetLastError(error);
    }
    return h;
}

extern "C" HANDLE WINAPI g6shim_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                            DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto p = g6shim::drives::translate(path);
    const HANDLE h = p ? CreateFileW(p->c_str(), access, share, security, disposition, flags, template_file)
                       : CreateFileA(path, access, share, security, disposition, flags, template_file);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        log_failure(p ? p->c_str() : widen(path).c_str(), error);
        SetLastError(error);
    }
    return h;
}

extern "C" HANDLE WINAPI g6shim_CreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                            LPCREATEFILE2_EXTENDED_PARAMETERS parameters) {
    const auto p = g6shim::drives::translate(path);
    const HANDLE h = CreateFile2(host_path(p, path), access, share, disposition, parameters);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        log_failure(host_path(p, path), error);
        SetLastError(error);
    }
    return h;
}

extern "C" HANDLE WINAPI g6shim_FindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
                                                 FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    return FindFirstFileExW(host_path(g6shim::drives::translate(path), path), level, data, op, filter, flags);
}

extern "C" HANDLE WINAPI g6shim_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    return FindFirstFileW(host_path(g6shim::drives::translate(path), path), data);
}

extern "C" HANDLE WINAPI g6shim_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data) {
    const auto p = g6shim::drives::translate(path);
    if (!p) return FindFirstFileA(path, data);
    WIN32_FIND_DATAW wide{};
    const HANDLE h = FindFirstFileW(p->c_str(), &wide);
    if (h != INVALID_HANDLE_VALUE && data) {
        std::memset(data, 0, sizeof *data);
        data->dwFileAttributes = wide.dwFileAttributes;
        data->ftCreationTime = wide.ftCreationTime;
        data->ftLastAccessTime = wide.ftLastAccessTime;
        data->ftLastWriteTime = wide.ftLastWriteTime;
        data->nFileSizeHigh = wide.nFileSizeHigh;
        data->nFileSizeLow = wide.nFileSizeLow;
        WideCharToMultiByte(CP_ACP, 0, wide.cFileName, -1, data->cFileName, MAX_PATH, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, wide.cAlternateFileName, -1, data->cAlternateFileName, 14, nullptr, nullptr);
    }
    return h;
}

extern "C" DWORD WINAPI g6shim_GetFileAttributesW(LPCWSTR path) {
    return GetFileAttributesW(host_path(g6shim::drives::translate(path), path));
}

extern "C" DWORD WINAPI g6shim_GetFileAttributesA(LPCSTR path) {
    const auto p = g6shim::drives::translate(path);
    return p ? GetFileAttributesW(p->c_str()) : GetFileAttributesA(path);
}

extern "C" BOOL WINAPI g6shim_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    return GetFileAttributesExW(host_path(g6shim::drives::translate(path), path), level, info);
}

extern "C" BOOL WINAPI g6shim_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    const auto p = g6shim::drives::translate(path);
    return p ? GetFileAttributesExW(p->c_str(), level, info) : GetFileAttributesExA(path, level, info);
}

extern "C" BOOL WINAPI g6shim_SetFileAttributesW(LPCWSTR path, DWORD attributes) {
    return SetFileAttributesW(host_path(g6shim::drives::translate(path), path), attributes);
}

extern "C" BOOL WINAPI g6shim_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    return CreateDirectoryW(host_path(g6shim::drives::translate(path), path), security);
}

extern "C" BOOL WINAPI g6shim_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    const auto p = g6shim::drives::translate(path);
    return p ? CreateDirectoryW(p->c_str(), security) : CreateDirectoryA(path, security);
}

extern "C" BOOL WINAPI g6shim_RemoveDirectoryW(LPCWSTR path) {
    return RemoveDirectoryW(host_path(g6shim::drives::translate(path), path));
}

extern "C" BOOL WINAPI g6shim_RemoveDirectoryA(LPCSTR path) {
    const auto p = g6shim::drives::translate(path);
    return p ? RemoveDirectoryW(p->c_str()) : RemoveDirectoryA(path);
}

extern "C" BOOL WINAPI g6shim_DeleteFileW(LPCWSTR path) {
    return DeleteFileW(host_path(g6shim::drives::translate(path), path));
}

extern "C" BOOL WINAPI g6shim_DeleteFileA(LPCSTR path) {
    const auto p = g6shim::drives::translate(path);
    return p ? DeleteFileW(p->c_str()) : DeleteFileA(path);
}

extern "C" BOOL WINAPI g6shim_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    return MoveFileExW(host_path(g6shim::drives::translate(from), from),
                       host_path(g6shim::drives::translate(to), to), flags);
}

namespace {

int redirect_imports(HMODULE module) {
    struct Redirect {
        const char* name;
        const void* to;
    };
    static const Redirect redirects[] = {
        {"CreateFileW", reinterpret_cast<const void*>(&g6shim_CreateFileW)},
        {"FindFirstFileExW", reinterpret_cast<const void*>(&g6shim_FindFirstFileExW)},
        {"GetFileAttributesExW", reinterpret_cast<const void*>(&g6shim_GetFileAttributesExW)},
        {"GetFileAttributesW", reinterpret_cast<const void*>(&g6shim_GetFileAttributesW)},
        {"SetFileAttributesW", reinterpret_cast<const void*>(&g6shim_SetFileAttributesW)},
        {"CreateDirectoryW", reinterpret_cast<const void*>(&g6shim_CreateDirectoryW)},
        {"RemoveDirectoryW", reinterpret_cast<const void*>(&g6shim_RemoveDirectoryW)},
        {"DeleteFileW", reinterpret_cast<const void*>(&g6shim_DeleteFileW)},
        {"MoveFileExW", reinterpret_cast<const void*>(&g6shim_MoveFileExW)},
    };
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (const auto& r : redirects) {
                if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), r.name) != 0) continue;
                DWORD old = 0;
                if (VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, PAGE_READWRITE, &old)) {
                    iat->u1.Function = reinterpret_cast<ULONGLONG>(r.to);
                    VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, old, &old);
                    ++patched;
                }
            }
        }
    }
    return patched;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        if (HMODULE crt = GetModuleHandleW(kCrt))
            G6SHIM_INFO("files: %d of ucrtbase's file imports translate G:\\ to the game folder", redirect_imports(crt));
    return TRUE;
}
