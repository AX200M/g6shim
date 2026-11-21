// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "capture.hpp"

#include <windows.h>
#include <d3d11.h>

#include <g6shim/log.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace g6shim::d3d {

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
        char spec[kMaxSpec] = "";
        if (!GetEnvironmentVariableA(kFramesVariable, spec, sizeof spec)) return out;
        for (char* p = spec; *p;) {
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

bool write_bmp(const std::wstring& path, const std::vector<std::uint8_t>& bgra, UINT width, UINT height) {
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
    bool ok = WriteFile(h, &file, sizeof file, &written, nullptr) && WriteFile(h, &info, sizeof info, &written, nullptr) &&
              WriteFile(h, bgra.data(), static_cast<DWORD>(bgra.size()), &written, nullptr);
    CloseHandle(h);
    return ok;
}

}  // namespace

void capture_if_requested(IDXGISwapChain* swap_chain, unsigned frame) {
    if (!swap_chain || !requested_frames().count(frame)) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.SampleDesc = {1, 0};
    ID3D11Texture2D* staging = nullptr;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if ((bgra || rgba) && desc.SampleDesc.Count == 1 && SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        context->CopyResource(staging, back);
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * kBytesPerPixel);
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                std::uint8_t* out = pixels.data() + static_cast<std::size_t>(y) * desc.Width * kBytesPerPixel;
                std::memcpy(out, row, static_cast<std::size_t>(desc.Width) * kBytesPerPixel);
                if (rgba)
                    for (UINT x = 0; x < desc.Width; ++x) std::swap(out[x * kBytesPerPixel + kRed], out[x * kBytesPerPixel + kBlue]);
                for (UINT x = 0; x < desc.Width; ++x) out[x * kBytesPerPixel + kAlpha] = kOpaque;
            }
            context->Unmap(staging, 0);
            const std::wstring path = output_path(frame);
            const bool ok = !path.empty() && write_bmp(path, pixels, desc.Width, desc.Height);
            G6SHIM_INFO("capture: frame %u (%ux%u) %s", frame, desc.Width, desc.Height, ok ? "saved" : "NOT saved");
        }
    } else {
        G6SHIM_WARN("capture: frame %u: back buffer format %d, %u samples not supported", frame,
                    static_cast<int>(desc.Format), desc.SampleDesc.Count);
    }
    if (staging) staging->Release();
    context->Release();
    device->Release();
    back->Release();
}

}  // namespace g6shim::d3d
