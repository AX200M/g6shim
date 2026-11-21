// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace g6shim::xg {

inline constexpr wchar_t kConsoleLibraryName[] = L"console_xg_x.dll";

namespace console_computer_slot {
inline constexpr std::size_t kRelease = 1;
inline constexpr std::size_t kGetResourceLayout = 2;
inline constexpr std::size_t kCopyFromSubresource = 9;
}  // namespace console_computer_slot

std::wstring console_library_path(const std::wstring& module_path);
HMODULE console_library();
FARPROC console_export(const char* name);

}  // namespace g6shim::xg
