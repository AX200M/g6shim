// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#if defined(G6SHIM_RUNTIME_BUILD)
#  define G6SHIM_DRIVES_API __declspec(dllexport)
#else
#  define G6SHIM_DRIVES_API __declspec(dllimport)
#endif

namespace g6shim::drives {

inline constexpr wchar_t kPackageDrive = L'G';
inline constexpr std::size_t kDrivePrefix = 3;
inline constexpr std::size_t kLongPath = 248;

template <class Char>
constexpr Char upper_letter(Char c) {
    return c >= Char('a') && c <= Char('z') ? static_cast<Char>(c - Char('a' - 'A')) : c;
}

template <class Char>
bool on_drive(const Char* path) {
    if (path == nullptr) return false;
    if (path[0] == Char('\\') && path[1] == Char('\\') && path[2] == Char('?') && path[3] == Char('\\'))
        path += 4;
    if (path[0] == Char('\0')) return false;
    const Char letter = upper_letter(path[0]);
    return letter >= Char('A') && letter <= Char('Z') && path[1] == Char(':');
}

G6SHIM_DRIVES_API std::size_t host_path_size(const wchar_t* path);
G6SHIM_DRIVES_API void host_path(const wchar_t* path, wchar_t* out);
G6SHIM_DRIVES_API std::size_t host_narrow_path_size(const char* path);
G6SHIM_DRIVES_API void host_narrow_path(const char* path, wchar_t* out);
G6SHIM_DRIVES_API std::size_t package_path_size(const wchar_t* path, const wchar_t* root);
G6SHIM_DRIVES_API void package_path(const wchar_t* path, const wchar_t* root, wchar_t* out);
G6SHIM_DRIVES_API int mount_drive(const wchar_t* root);
G6SHIM_DRIVES_API int persistent_storage_drive();
G6SHIM_DRIVES_API void mount_drive_at(wchar_t letter, const wchar_t* root);

template <class Char>
bool on_package_drive(const Char* path) {
    return on_drive(path) && upper_letter(path[0]) == kPackageDrive &&
           (path[2] == Char('\\') || path[2] == Char('/'));
}

inline std::optional<std::wstring> translate(const wchar_t* path) {
    const std::size_t size = host_path_size(path);
    if (size == 0) return std::nullopt;
    std::wstring out(size - 1, L'\0');
    host_path(path, out.data());
    return out;
}

inline std::optional<std::wstring> translate(const char* path) {
    const std::size_t size = host_narrow_path_size(path);
    if (size == 0) return std::nullopt;
    std::wstring out(size - 1, L'\0');
    host_narrow_path(path, out.data());
    return out;
}

inline std::wstring translate_package(const wchar_t* path, const wchar_t* root) {
    const std::size_t size = package_path_size(path, root);
    if (size == 0) return path != nullptr ? std::wstring(path) : std::wstring{};
    std::wstring out(size - 1, L'\0');
    package_path(path, root, out.data());
    return out;
}

}  // namespace g6shim::drives
