// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "game_drive.hpp"

using g6shim::kernelx::on_game_drive;
using g6shim::kernelx::translate_game_drive;

TEST(game_drive_recognises_paths) {
    CHECK(on_game_drive("G:\\data\\file.bin"));
    CHECK(on_game_drive("g:/data"));
    CHECK(on_game_drive(L"G:\\"));
    CHECK(!on_game_drive("G:"));
    CHECK(!on_game_drive("G:data"));
    CHECK(!on_game_drive("C:\\data"));
    CHECK(!on_game_drive(""));
    CHECK(!on_game_drive(static_cast<const char*>(nullptr)));
}

TEST(game_drive_translates_to_the_game_folder) {
    const std::wstring folder = L"D:\\games\\copy\\";
    CHECK(translate_game_drive(L"G:\\GPGPU\\GPGPU.BIN", folder) == L"D:\\games\\copy\\GPGPU\\GPGPU.BIN");
    CHECK(translate_game_drive(L"g:/a/b", folder) == L"D:\\games\\copy\\a/b");
    CHECK(translate_game_drive(L"C:\\other", folder) == L"C:\\other");
    CHECK(translate_game_drive(L"G:\\x", L"") == L"G:\\x");
    CHECK(translate_game_drive(nullptr, folder).empty());
}
