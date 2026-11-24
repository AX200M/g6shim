// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/drives.hpp>

#include <string>

namespace g6shim::kernelx {

template <class Char>
bool on_game_drive(const Char* path) {
    return g6shim::drives::on_package_drive(path);
}

inline std::wstring translate_game_drive(const wchar_t* path, const std::wstring& game_folder) {
    if (path == nullptr) return {};
    return g6shim::drives::translate_package(path, game_folder.c_str());
}

}  // namespace g6shim::kernelx
