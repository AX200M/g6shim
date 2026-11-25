// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace g6shim::drives::detail {

inline constexpr int kFolderRemoveAttempts = 16;
inline constexpr DWORD kFolderRemovePauseMs = 25;

inline void remove_folder_with_retries(const std::wstring& root) {
    for (int attempt = 0; attempt < kFolderRemoveAttempts; ++attempt) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (!error) return;
        if (attempt + 1 != kFolderRemoveAttempts) Sleep(kFolderRemovePauseMs);
    }
}

}  // namespace g6shim::drives::detail
