// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "computer.hpp"
#include "layout.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <memory>

using namespace g6shim::xg;

namespace {

Texture2DDesc texture(std::uint32_t width, std::uint32_t height, std::uint32_t mips, DXGI_FORMAT format) {
    Texture2DDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.mip_levels = mips;
    desc.array_size = 1;
    desc.format = static_cast<std::uint32_t>(format);
    desc.sample_count = 1;
    desc.bind_flags = 8;
    desc.tile_mode = kTileModeLinearGeneral;
    return desc;
}

ID3D12Device* warp_device() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (adapter) adapter->Release();
    factory->Release();
    return device;
}

bool matches_pc_footprints(ID3D12Device* device, const Texture2DDesc& desc) {
    auto layout = std::make_unique<ResourceLayout>();
    if (FAILED(compute_linear_layout(desc, layout.get()))) return false;
    FormatInfo format{};
    format_info(desc.format, &format);
    D3D12_RESOURCE_DESC pc{};
    pc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pc.Width = desc.width;
    pc.Height = desc.height;
    pc.DepthOrArraySize = 1;
    pc.MipLevels = static_cast<UINT16>(layout->mip_levels);
    pc.Format = static_cast<DXGI_FORMAT>(desc.format);
    pc.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kMaxMipLevels]{};
    UINT rows[kMaxMipLevels]{};
    device->GetCopyableFootprints(&pc, 0, layout->mip_levels, 0, footprints, rows, nullptr, nullptr);
    for (std::uint32_t level = 0; level < layout->mip_levels; ++level) {
        const MipLayout& mip = layout->plane[0].mips[level];
        const D3D12_SUBRESOURCE_FOOTPRINT& fp = footprints[level].Footprint;
        if (mip.offset_bytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) return false;
        if (mip.pitch_bytes < fp.RowPitch || mip.pitch_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) return false;
        if (mip.height_elements != rows[level]) return false;
        if (mip.width_elements * format.block_width != fp.Width) return false;
        if (mip.slice_2d_size_bytes < std::uint64_t{mip.pitch_bytes} * rows[level]) return false;
    }
    return true;
}

}  // namespace

TEST(xg_layout_is_linear_with_pc_pitches) {
    auto layout = std::make_unique<ResourceLayout>();
    CHECK(compute_linear_layout(texture(256, 256, 0, DXGI_FORMAT_R8G8B8A8_UNORM), layout.get()) == S_OK);
    CHECK(layout->mip_levels == 9 && layout->planes == 1 && layout->dimension == kResourceDimensionTexture2D);
    const PlaneLayout& plane = layout->plane[0];
    CHECK(plane.bytes_per_element == 4);
    CHECK(plane.mips[0].pitch_bytes == 1024 && plane.mips[0].offset_bytes == 0 && plane.mips[0].size_bytes == 262144);
    CHECK(plane.mips[1].pitch_bytes == 512 && plane.mips[1].offset_bytes == 262144);
    CHECK(plane.mips[8].width_elements == 1 && plane.mips[8].pitch_bytes == kRowPitchAlignment);
    CHECK(plane.mips[0].tile_mode == kTileModeLinearGeneral);
    CHECK(plane.mips[9].size_bytes == 0);
    CHECK(layout->size_bytes == plane.mips[8].offset_bytes + plane.mips[8].size_bytes);
}

TEST(xg_layout_counts_blocks_for_compressed_formats) {
    auto layout = std::make_unique<ResourceLayout>();
    CHECK(compute_linear_layout(texture(256, 256, 1, DXGI_FORMAT_BC1_UNORM), layout.get()) == S_OK);
    const MipLayout& mip = layout->plane[0].mips[0];
    CHECK(layout->plane[0].bytes_per_element == 8);
    CHECK(mip.width_elements == 64 && mip.height_elements == 64 && mip.pitch_bytes == 512 && mip.pitch_pixels == 256);
    CHECK(compute_linear_layout(texture(13, 7, 0, DXGI_FORMAT_BC3_UNORM), layout.get()) == S_OK);
    CHECK(layout->mip_levels == 4 && layout->plane[0].mips[0].width_elements == 4);
    CHECK(layout->plane[0].mips[3].width_elements == 1 && layout->plane[0].mips[3].height_elements == 1);
}

TEST(xg_layout_rejects_what_it_cannot_describe) {
    auto layout = std::make_unique<ResourceLayout>();
    CHECK(compute_linear_layout(texture(256, 256, 1, DXGI_FORMAT_NV12), layout.get()) == E_INVALIDARG);
    CHECK(compute_linear_layout(texture(0, 256, 1, DXGI_FORMAT_R8_UNORM), layout.get()) == E_INVALIDARG);
    CHECK(compute_linear_layout(texture(4, 4, 4, DXGI_FORMAT_R8_UNORM), layout.get()) == E_INVALIDARG);
    CHECK(compute_linear_layout(texture(4, 4, 1, DXGI_FORMAT_R8_UNORM), nullptr) == E_POINTER);
}

TEST(xg_layout_fits_pc_copyable_footprints) {
    ID3D12Device* device = warp_device();
    if (!device) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    CHECK(matches_pc_footprints(device, texture(256, 256, 0, DXGI_FORMAT_R8G8B8A8_UNORM)));
    CHECK(matches_pc_footprints(device, texture(100, 60, 3, DXGI_FORMAT_R8G8B8A8_UNORM)));
    CHECK(matches_pc_footprints(device, texture(256, 128, 0, DXGI_FORMAT_BC1_UNORM)));
    CHECK(matches_pc_footprints(device, texture(13, 7, 0, DXGI_FORMAT_BC7_UNORM)));
    CHECK(matches_pc_footprints(device, texture(37, 19, 0, DXGI_FORMAT_R16_FLOAT)));
    device->Release();
}

TEST(xg_texture_computer_hands_out_its_layout) {
    const Texture2DDesc desc = texture(64, 32, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    TextureComputer* computer = nullptr;
    CHECK(create_texture_2d_computer(&desc, &computer) == S_OK && computer);
    if (!computer) return;
    using GetLayout = HRESULT(STDMETHODCALLTYPE*)(TextureComputer*, ResourceLayout*);
    using Count = ULONG(STDMETHODCALLTYPE*)(TextureComputer*);
    auto layout = std::make_unique<ResourceLayout>();
    CHECK(reinterpret_cast<GetLayout>(computer->table[computer_slot::kGetResourceLayout])(computer, layout.get()) == S_OK);
    CHECK(layout->mip_levels == 7 && layout->plane[0].mips[0].pitch_bytes == 256);
    CHECK(reinterpret_cast<Count>(computer->table[computer_slot::kAddRef])(computer) == 2);
    CHECK(reinterpret_cast<Count>(computer->table[computer_slot::kRelease])(computer) == 1);
    CHECK(reinterpret_cast<Count>(computer->table[computer_slot::kRelease])(computer) == 0);
    const Texture2DDesc bad = texture(64, 32, 0, DXGI_FORMAT_UNKNOWN);
    CHECK(create_texture_2d_computer(&bad, &computer) == E_INVALIDARG && !computer);
}
