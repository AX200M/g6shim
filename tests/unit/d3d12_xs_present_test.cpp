// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

constexpr UINT kSceneWidth = 64;
constexpr UINT kSceneHeight = 36;
constexpr UINT kOverlayWidth = 128;
constexpr UINT kOverlayHeight = 72;
constexpr UINT kBytesPerPixel = 4;
constexpr int kTolerance = 1;

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

struct Pixel {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
};

Pixel colour(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha) {
    return {red, green, blue, alpha};
}

Pixel scene_colour() { return colour(220, 30, 40, 255); }

Pixel overlay_opaque() { return colour(10, 200, 50, 255); }

Pixel overlay_half() { return colour(0, 128, 0, 128); }

void write_pixel(std::vector<std::uint8_t>& pixels, UINT width, UINT x, UINT y, Pixel pixel) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
    pixels[at] = pixel.red;
    pixels[at + 1] = pixel.green;
    pixels[at + 2] = pixel.blue;
    pixels[at + 3] = pixel.alpha;
}

std::vector<std::uint8_t> scene_pixels() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSceneWidth) * kSceneHeight * kBytesPerPixel);
    const Pixel filled = scene_colour();
    for (UINT y = 0; y < kSceneHeight; ++y) {
        for (UINT x = 0; x < kSceneWidth; ++x) write_pixel(pixels, kSceneWidth, x, y, filled);
    }
    return pixels;
}

std::vector<std::uint8_t> overlay_pixels() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kOverlayWidth) * kOverlayHeight * kBytesPerPixel);
    for (UINT y = 0; y < kOverlayHeight; ++y) {
        for (UINT x = 0; x < kOverlayWidth; ++x) {
            Pixel filled{};
            if (y >= kOverlayHeight / 2)
                filled = overlay_half();
            else if (x >= kOverlayWidth / 2)
                filled = overlay_opaque();
            write_pixel(pixels, kOverlayWidth, x, y, filled);
        }
    }
    write_pixel(pixels, kOverlayWidth, 32, 16, colour(255, 0, 0, 255));
    write_pixel(pixels, kOverlayWidth, 33, 16, colour(0, 255, 0, 255));
    write_pixel(pixels, kOverlayWidth, 32, 17, colour(0, 0, 255, 255));
    write_pixel(pixels, kOverlayWidth, 33, 17, colour(255, 255, 0, 255));
    return pixels;
}

D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

Microsoft::WRL::ComPtr<ID3D12Resource> make_default(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                                    D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&texture))))
        return nullptr;
    return texture;
}

bool upload(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
            const std::vector<std::uint8_t>& pixels, UINT width, UINT height,
            Microsoft::WRL::ComPtr<ID3D12Resource>& kept) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(footprint.Footprint.RowPitch) * height;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&kept))))
        return false;
    void* mapped = nullptr;
    if (FAILED(kept->Map(0, nullptr, &mapped))) return false;
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<std::size_t>(y) * width * kBytesPerPixel,
                    static_cast<std::size_t>(width) * kBytesPerPixel);
    }
    kept->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{kept.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    source.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return true;
}

int blended(int under, int over, int over_alpha) {
    const double value = over + under * (1.0 - static_cast<double>(over_alpha) / 255.0);
    return static_cast<int>(std::lround(value));
}

bool within(std::uint8_t got, int expected, int tolerance) {
    return std::abs(static_cast<int>(got) - expected) <= tolerance;
}

void check_pixel(const std::uint8_t* pixels, UINT row_pitch, UINT x, UINT y, int red, int green, int blue,
                 int tolerance) {
    const std::uint8_t* pixel =
        pixels + static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * kBytesPerPixel;
    CHECK(within(pixel[0], red, tolerance));
    CHECK(within(pixel[1], green, tolerance));
    CHECK(within(pixel[2], blue, tolerance));
    CHECK(pixel[3] == 255);
}

}  // namespace

TEST(d3d12_xs_present_composes_scaled_planes_over_the_first) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    host::Host* gpu = host::host();
    if (gpu == nullptr) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    ID3D12Device* device = gpu->device();
    const auto scene = make_default(device,
                                    texture_desc(kSceneWidth, kSceneHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 D3D12_RESOURCE_FLAG_NONE),
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    const auto overlay = make_default(device,
                                      texture_desc(kOverlayWidth, kOverlayHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                   D3D12_RESOURCE_FLAG_NONE),
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    const auto target = make_default(device,
                                     texture_desc(kSceneWidth, kSceneHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(scene != nullptr);
    CHECK(overlay != nullptr);
    CHECK(target != nullptr);
    if (!scene || !overlay || !target) return;

    auto commands = gpu->new_list();
    CHECK(commands != nullptr);
    if (!commands) return;
    ID3D12GraphicsCommandList* list = commands->list();
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> overlay_upload;
    CHECK(upload(device, list, scene.Get(), scene_pixels(), kSceneWidth, kSceneHeight, scene_upload));
    CHECK(upload(device, list, overlay.Get(), overlay_pixels(), kOverlayWidth, kOverlayHeight, overlay_upload));
    const D3D12_RESOURCE_BARRIER to_shader[] = {
        host::transition(scene.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        host::transition(overlay.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, to_shader);
    const host::PresentedPlane planes[] = {
        {scene.Get(), static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)},
        {overlay.Get(), static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)},
    };
    CHECK(host::compose(list, planes, target.Get()));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const D3D12_RESOURCE_DESC target_desc = target->GetDesc();
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback))));
    if (!readback) return;
    const D3D12_RESOURCE_BARRIER to_copy =
        host::transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->close();
    gpu->execute({commands.get()});
    CHECK(device->GetDeviceRemovedReason() == S_OK);

    void* mapped = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    if (mapped == nullptr) return;
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    const UINT pitch = footprint.Footprint.RowPitch;
    const Pixel under = scene_colour();
    const Pixel opaque = overlay_opaque();
    check_pixel(pixels, pitch, 8, 8, under.red, under.green, under.blue, 0);
    check_pixel(pixels, pitch, 48, 8, opaque.red, opaque.green, opaque.blue, 0);
    check_pixel(pixels, pitch, 16, 8, 128, 128, 64, kTolerance);
    const Pixel half = overlay_half();
    check_pixel(pixels, pitch, 8, 27, blended(under.red, half.red, half.alpha),
                blended(under.green, half.green, half.alpha), blended(under.blue, half.blue, half.alpha), kTolerance);
    check_pixel(pixels, pitch, 48, 27, blended(under.red, half.red, half.alpha),
                blended(under.green, half.green, half.alpha), blended(under.blue, half.blue, half.alpha), kTolerance);
    readback->Unmap(0, nullptr);
}
