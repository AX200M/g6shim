// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/ambient.hpp"
#include "d3d12_xs/host.hpp"

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace g6shim::d3d12_xs;
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 32;
constexpr std::uint32_t kRowPitch = 256;

host::Host* ambient_host() {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    return host::host();
}

std::uint32_t pack_shared_exponent(float r, float g, float b) {
    const float top = std::max(r, std::max(g, b));
    int e = std::clamp(static_cast<int>(std::floor(std::log2(std::max(top, 1e-9f)))) + 16, 0, 31);
    float scale = std::exp2(static_cast<float>(24 - e));
    if (top * scale >= 511.5f) {
        e = std::min(e + 1, 31);
        scale = std::exp2(static_cast<float>(24 - e));
    }
    const auto m = [&](float c) { return std::min(static_cast<std::uint32_t>(c * scale + 0.5f), 511u); };
    return m(r) | m(g) << 9 | m(b) << 18 | static_cast<std::uint32_t>(e) << 27;
}

ComPtr<ID3D12Resource> plain_texture(ID3D12Device* device, DXGI_FORMAT format, bool unordered, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, std::uint64_t bytes,
                              D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

D3D12_TEXTURE_COPY_LOCATION footprint(ID3D12Resource* resource, DXGI_FORMAT format) {
    D3D12_TEXTURE_COPY_LOCATION at{};
    at.pResource = resource;
    at.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    at.PlacedFootprint.Footprint.Format = format;
    at.PlacedFootprint.Footprint.Width = kWidth;
    at.PlacedFootprint.Footprint.Height = kHeight;
    at.PlacedFootprint.Footprint.Depth = 1;
    at.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    return at;
}

D3D12_TEXTURE_COPY_LOCATION subresource(ID3D12Resource* resource) {
    D3D12_TEXTURE_COPY_LOCATION at{};
    at.pResource = resource;
    at.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return at;
}

}  // namespace

TEST(d3d12_xs_ambient_settings_parse_levels_and_roles) {
    CHECK(ambient::parse_settings("", "").mode == ambient::Mode::kOff);
    CHECK(ambient::parse_settings("0", "").mode == ambient::Mode::kOff);
    CHECK(ambient::parse_settings("off", "").mode == ambient::Mode::kOff);
    CHECK(ambient::parse_settings("constant:nan", "").mode == ambient::Mode::kOff);
    const ambient::Settings estimate = ambient::parse_settings("0.8", "");
    CHECK(estimate.mode == ambient::Mode::kEstimate);
    CHECK(estimate.level == 0.8f);
    const ambient::Settings constant = ambient::parse_settings("constant:0.3", "");
    CHECK(constant.mode == ambient::Mode::kConstant);
    CHECK(constant.level == 0.3f);

    const ambient::Settings set = ambient::parse_settings("1",
                                                         "# a comment\n"
                                                         "resolve RESOLVE_NAME 1 176 0 464\n"
                                                         "radiance RADIANCE_NAME 1 16\n"
                                                         "air AIR_NAME 1 32\n"
                                                         "resolve MISSING_DEPTH 1 176\n"
                                                         "camera 0.2 0.5\n"
                                                         "shade 0 1.5\n"
                                                         "settle 0 2 1\n"
                                                         "air-cap 3\n");
    CHECK(set.roles.size() == 3);
    if (set.roles.size() == 3) {
        CHECK(set.roles[0].role == ambient::Role::kResolve);
        CHECK(set.roles[0].name == "RESOLVE_NAME");
        CHECK(set.roles[0].image.slot == 1);
        CHECK(set.roles[0].image.offset_dwords == 176);
        CHECK(set.roles[0].depth.has_value());
        if (set.roles[0].depth) {
            CHECK(set.roles[0].depth->slot == 0);
            CHECK(set.roles[0].depth->offset_dwords == 464);
        }
        CHECK(set.roles[1].role == ambient::Role::kRadiance);
        CHECK(!set.roles[1].depth.has_value());
        CHECK(set.roles[2].role == ambient::Role::kAir);
        CHECK(set.roles[2].image.offset_dwords == 32);
    }
    CHECK(set.near_plane == 0.2f);
    CHECK(set.tan_half_y == 0.5f);
    CHECK(set.shade[0] == 0.0f);
    CHECK(set.shade[1] == 1.5f);
    CHECK(set.settle_ms == 0.0f);
    CHECK(set.jump == 2.0f);
    CHECK(set.fresh == 1.0f);
    CHECK(set.air_cap == 3.0f);
}

TEST(d3d12_xs_ambient_checks_the_images_it_changes) {
    D3D12_RESOURCE_DESC light{};
    light.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    light.Width = kWidth;
    light.Height = kHeight;
    light.Format = DXGI_FORMAT_R32_UINT;
    D3D12_RESOURCE_DESC depth = light;
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    CHECK(ambient::image_fits(ambient::Role::kResolve, light, &depth));
    CHECK(!ambient::image_fits(ambient::Role::kResolve, light, nullptr));
    D3D12_RESOURCE_DESC smaller = depth;
    smaller.Width = kWidth / 2;
    CHECK(!ambient::image_fits(ambient::Role::kResolve, light, &smaller));
    D3D12_RESOURCE_DESC radiance = light;
    radiance.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    CHECK(ambient::image_fits(ambient::Role::kRadiance, radiance, nullptr));
    CHECK(!ambient::image_fits(ambient::Role::kRadiance, light, nullptr));
    D3D12_RESOURCE_DESC volume = radiance;
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    CHECK(ambient::image_fits(ambient::Role::kAir, volume, nullptr));
    CHECK(!ambient::image_fits(ambient::Role::kAir, radiance, nullptr));
}

TEST(d3d12_xs_ambient_constant_light_reaches_every_texel) {
    host::Host* gpu = ambient_host();
    if (gpu == nullptr || gpu->device() == nullptr) return;
    CHECK(ambient::passes_ready());
    if (!ambient::passes_ready()) return;
    ID3D12Device* device = gpu->device();
    const auto depth = plain_texture(device, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto light = plain_texture(device, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::uint64_t bytes = static_cast<std::uint64_t>(kRowPitch) * kHeight;
    const auto upload = buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto readback = buffer(device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK((depth && light && upload && readback));
    if (!depth || !light || !upload || !readback) return;
    void* mapped = nullptr;
    CHECK(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
    if (mapped == nullptr) return;
    std::vector<float> ones(bytes / sizeof(float), 1.0f);
    std::memcpy(mapped, ones.data(), bytes);
    upload->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))));
    if (!heap) return;
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth.Get(), &srv, cpu);
    cpu.ptr += step;
    device->CreateUnorderedAccessView(light.Get(), nullptr, nullptr, cpu);

    const auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    ID3D12GraphicsCommandList* commands = list->list();
    const D3D12_TEXTURE_COPY_LOCATION from = footprint(upload.Get(), DXGI_FORMAT_R32_FLOAT);
    const D3D12_TEXTURE_COPY_LOCATION to = subresource(depth.Get());
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(commands, depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    ambient::Settings set;
    set.mode = ambient::Mode::kConstant;
    set.level = 1.0f;
    set.shade = {0.0f, 0.6f};
    ambient::Views views;
    views.depth = heap->GetGPUDescriptorHandleForHeapStart();
    views.image = D3D12_GPU_DESCRIPTOR_HANDLE{views.depth.ptr + step};
    views.width = kWidth;
    views.height = kHeight;
    CHECK(ambient::record(*list, ambient::Role::kResolve, views, set));
    transition(commands, light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION source = subresource(light.Get());
    const D3D12_TEXTURE_COPY_LOCATION back = footprint(readback.Get(), DXGI_FORMAT_R32_UINT);
    commands->CopyTextureRegion(&back, 0, 0, 0, &source, nullptr);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();

    void* read = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, &read)));
    if (read == nullptr) return;
    const std::uint32_t expected = pack_shared_exponent(1.0f, 0.98f, 0.94f);
    std::uint32_t wrong = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(read) + y * kRowPitch);
        for (std::uint32_t x = 0; x < kWidth; ++x)
            if (row[x] != expected) ++wrong;
    }
    readback->Unmap(0, nullptr);
    CHECK(wrong == 0);
}
