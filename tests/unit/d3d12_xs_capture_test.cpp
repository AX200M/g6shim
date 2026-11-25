// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

constexpr unsigned kCapturedRgbaFrame = 990001;
constexpr unsigned kCapturedBgraFrame = 990002;
constexpr unsigned kSkippedFrame = 990003;
constexpr UINT kWidth = 8;
constexpr UINT kHeight = 4;
constexpr std::size_t kBytesPerPixel = 4;
constexpr std::size_t kHeaderBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
constexpr DWORD kQueueTimeout = 5000;

struct CaptureEnvironment {
    CaptureEnvironment() {
        ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
        ::SetEnvironmentVariableA("G6SHIM_CAPTURE_FRAMES", "990001,990002");
    }
};

const CaptureEnvironment g_capture_environment;

std::wstring frame_path(unsigned frame) {
    wchar_t exe[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path.erase(path.find_last_of(L"\\/") + 1);
    return path + L"g6shim_frame_" + std::to_wstring(frame) + L".bmp";
}

bool submit_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandList* list) {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    const HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return false;
    queue->ExecuteCommandLists(1, &list);
    const bool done = SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                      SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                      ::WaitForSingleObject(event, kQueueTimeout) == WAIT_OBJECT_0;
    ::CloseHandle(event);
    return done;
}

D3D12_RESOURCE_DESC texture_description(DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

Microsoft::WRL::ComPtr<ID3D12Resource> make_texture(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                    DXGI_FORMAT format, const std::vector<std::uint8_t>& pixels) {
    const D3D12_RESOURCE_DESC desc = texture_description(format);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = static_cast<UINT64>(footprint.Footprint.RowPitch) * kHeight;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return nullptr;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return nullptr;
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(static_cast<std::uint8_t*>(mapped) + footprint.Offset + static_cast<std::size_t>(y) *
                                                                             footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<std::size_t>(y) * kWidth * kBytesPerPixel, kWidth * kBytesPerPixel);
    upload->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
        return nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        return nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return nullptr;
    D3D12_TEXTURE_COPY_LOCATION destination{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    source.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const D3D12_RESOURCE_BARRIER barrier =
        host::transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close()) || !submit_and_wait(device, queue, list.Get())) return nullptr;
    return texture;
}

std::vector<std::uint8_t> pattern(DXGI_FORMAT format) {
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<std::uint8_t> pixels(kWidth * kHeight * kBytesPerPixel);
    for (std::size_t i = 0; i < pixels.size(); i += kBytesPerPixel) {
        const std::size_t n = i / kBytesPerPixel;
        const std::uint8_t red = static_cast<std::uint8_t>(n * 7 + 3);
        const std::uint8_t green = static_cast<std::uint8_t>(n * 11 + 5);
        const std::uint8_t blue = static_cast<std::uint8_t>(n * 13 + 9);
        pixels[i] = rgba ? red : blue;
        pixels[i + 1] = green;
        pixels[i + 2] = rgba ? blue : red;
        pixels[i + 3] = 0;
    }
    return pixels;
}

std::vector<std::uint8_t> read_bytes(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool exists(const std::wstring& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void check_frame(unsigned frame, DXGI_FORMAT format) {
    const std::vector<std::uint8_t> pixels = pattern(format);
    const std::wstring path = frame_path(frame);
    ::DeleteFileW(path.c_str());
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    const auto texture = make_texture(gpu->device(), gpu->queue(), format, pixels);
    CHECK(texture != nullptr);
    if (!texture) return;
    const auto presented =
        gpu->present(texture.Get(), static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON), frame);
    CHECK(presented.has_value());
    CHECK(presented && *presented == false);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    const std::vector<std::uint8_t> bytes = read_bytes(path);
    CHECK(bytes.size() == kHeaderBytes + pixels.size());
    if (bytes.size() != kHeaderBytes + pixels.size()) {
        ::DeleteFileW(path.c_str());
        return;
    }
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    std::memcpy(&header, bytes.data(), sizeof header);
    std::memcpy(&info, bytes.data() + sizeof header, sizeof info);
    CHECK(header.bfType == 0x4D42);
    CHECK(info.biWidth == static_cast<LONG>(kWidth));
    CHECK(info.biHeight == -static_cast<LONG>(kHeight));
    CHECK(info.biBitCount == 32);
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i) {
        const std::uint8_t* source = pixels.data() + i * kBytesPerPixel;
        const std::uint8_t* saved = bytes.data() + kHeaderBytes + i * kBytesPerPixel;
        CHECK(saved[0] == source[rgba ? 2 : 0]);
        CHECK(saved[1] == source[1]);
        CHECK(saved[2] == source[rgba ? 0 : 2]);
        CHECK(saved[3] == 0xFF);
    }
    ::DeleteFileW(path.c_str());
}

}  // namespace

TEST(d3d12_xs_present_captures_requested_frames) {
    check_frame(kCapturedRgbaFrame, DXGI_FORMAT_R8G8B8A8_UNORM);
    check_frame(kCapturedBgraFrame, DXGI_FORMAT_B8G8R8A8_UNORM);
}

TEST(d3d12_xs_present_skips_frames_not_requested) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    const std::wstring path = frame_path(kSkippedFrame);
    ::DeleteFileW(path.c_str());
    const std::vector<std::uint8_t> pixels = pattern(DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto texture = make_texture(gpu->device(), gpu->queue(), DXGI_FORMAT_R8G8B8A8_UNORM, pixels);
    CHECK(texture != nullptr);
    if (!texture) return;
    const auto presented =
        gpu->present(texture.Get(), static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON), kSkippedFrame);
    CHECK(presented.has_value());
    CHECK(!exists(path));
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
}
