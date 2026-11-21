// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include <g6shim/capture.hpp>

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr unsigned kTestFrame = 999999;
constexpr std::size_t kHeaderBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

std::wstring frame_path() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path.erase(path.find_last_of(L"\\/") + 1);
    return path + L"g6shim_frame_" + std::to_wstring(kTestFrame) + L".bmp";
}

}  // namespace

TEST(captured_frames_become_opaque_bgra_bitmaps) {
    const std::uint8_t rgba[] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0, 0, 0};
    CHECK(g6shim::save_captured_frame(kTestFrame, rgba, 2, 2, 12, g6shim::PixelOrder::Rgba));
    std::ifstream file(frame_path(), std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    CHECK(bytes.size() == kHeaderBytes + 16);
    if (bytes.size() == kHeaderBytes + 16) {
        CHECK(bytes[0] == 'B' && bytes[1] == 'M');
        const std::uint8_t* pixels = bytes.data() + kHeaderBytes;
        CHECK(pixels[0] == 3 && pixels[1] == 2 && pixels[2] == 1 && pixels[3] == 0xFF);
        CHECK(pixels[8] == 11 && pixels[11] == 0xFF);
    }
    DeleteFileW(frame_path().c_str());
    CHECK(!g6shim::save_captured_frame(kTestFrame, rgba, 2, 2, 4, g6shim::PixelOrder::Rgba));
}
