// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/capture.hpp"
#include <g6shim/settings.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace g6shim {

namespace {

constexpr char kFramesVariable[] = "G6SHIM_CAPTURE_FRAMES";
constexpr std::size_t kMaxSpec = 512;
constexpr WORD kBitmapSignature = 0x4D42;
constexpr WORD kBitsPerPixel = 32;
constexpr std::size_t kBytesPerPixel = 4;
constexpr std::size_t kRed = 0;
constexpr std::size_t kBlue = 2;
constexpr std::size_t kAlpha = 3;
constexpr std::uint8_t kOpaque = 0xFF;

const std::set<unsigned>& requested_frames() {
    static const std::set<unsigned> frames = [] {
        std::set<unsigned> out;
        const std::optional<std::string> value = settings::get(kFramesVariable);
        if (!value) return out;
        std::string spec = *value;
        if (spec.size() >= kMaxSpec) spec.resize(kMaxSpec - 1);
        for (char* p = spec.data(); *p;) {
            char* next = nullptr;
            const unsigned long n = std::strtoul(p, &next, 10);
            if (next == p) {
                ++p;
                continue;
            }
            out.insert(static_cast<unsigned>(n));
            p = next;
        }
        return out;
    }();
    return frames;
}

std::wstring output_path(unsigned frame) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring path(exe, n);
    path.erase(path.find_last_of(L"\\/") + 1);
    return path + L"g6shim_frame_" + std::to_wstring(frame) + L".bmp";
}

bool write_bmp(const std::wstring& path, const std::vector<std::uint8_t>& bgra, unsigned width, unsigned height) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof info;
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = kBitsPerPixel;
    info.biCompression = BI_RGB;
    file.bfType = kBitmapSignature;
    file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = static_cast<DWORD>(file.bfOffBits + bgra.size());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, &file, sizeof file, &written, nullptr) &&
                    WriteFile(h, &info, sizeof info, &written, nullptr) &&
                    WriteFile(h, bgra.data(), static_cast<DWORD>(bgra.size()), &written, nullptr);
    CloseHandle(h);
    return ok;
}

}  // namespace

bool capture_requested(unsigned frame) { return requested_frames().count(frame) != 0; }

bool save_captured_frame(unsigned frame, const void* pixels, unsigned width, unsigned height, std::size_t row_pitch,
                         PixelOrder order) {
    if (!pixels || width == 0 || height == 0 || row_pitch < std::size_t{width} * kBytesPerPixel) return false;
    std::vector<std::uint8_t> bgra(std::size_t{width} * height * kBytesPerPixel);
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(pixels) + std::size_t{y} * row_pitch;
        std::uint8_t* out = bgra.data() + std::size_t{y} * width * kBytesPerPixel;
        std::memcpy(out, row, std::size_t{width} * kBytesPerPixel);
        for (unsigned x = 0; x < width; ++x) {
            std::uint8_t* pixel = out + std::size_t{x} * kBytesPerPixel;
            if (order == PixelOrder::Rgba) std::swap(pixel[kRed], pixel[kBlue]);
            pixel[kAlpha] = kOpaque;
        }
    }
    const std::wstring path = output_path(frame);
    const bool ok = !path.empty() && write_bmp(path, bgra, width, height);
    log(ok ? Level::Info : Level::Warn, "capture: frame %u (%ux%u) %s", frame, width, height,
        ok ? "saved" : "NOT saved");
    return ok;
}

}  // namespace g6shim
