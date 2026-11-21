// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace g6shim::kernelx {

inline constexpr std::size_t kGameDrivePrefix = 3;

template <class Char>
bool on_game_drive(const Char* path) {
    return path && (path[0] == Char('G') || path[0] == Char('g')) && path[1] == Char(':') &&
           (path[2] == Char('\\') || path[2] == Char('/'));
}

inline std::wstring translate_game_drive(const wchar_t* path, const std::wstring& game_folder) {
    if (!path) return {};
    if (on_game_drive(path) && !game_folder.empty()) return game_folder + (path + kGameDrivePrefix);
    return path;
}

}  // namespace g6shim::kernelx
