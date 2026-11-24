// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "computer.hpp"
#include "console_xg.hpp"
#include "layout.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <memory>
#include <vector>

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

namespace {

Texture2DDesc tiled(std::uint32_t width, std::uint32_t height, std::uint32_t mips, DXGI_FORMAT format,
                    std::uint32_t slices = 1) {
    Texture2DDesc desc = texture(width, height, mips, format);
    desc.array_size = slices;
    desc.tile_mode = kTileModeThin1D;
    return desc;
}

}  // namespace

TEST(xg_picks_thin_tiling_for_2d_textures) {
    CHECK(optimal_tile_mode(kResourceDimensionTexture2D, 1) == kTileModeThin1D);
    CHECK(optimal_tile_mode(kResourceDimensionTexture2D, 4) == kTileModeLinearGeneral);
    CHECK(optimal_tile_mode(2, 1) == kTileModeLinearGeneral);
}

TEST(xg_tiled_layouts_match_the_console) {
    auto layout = std::make_unique<ResourceLayout>();
    CHECK(compute_layout(tiled(100, 60, 1, DXGI_FORMAT_R8G8B8A8_UNORM), layout.get()) == S_OK);
    const MipLayout& single = layout->plane[0].mips[0];
    CHECK(layout->size_bytes == 26624 && single.padded_width_elements == 104 && single.padded_height_elements == 64);
    CHECK(single.pitch_bytes == 416 && single.tile_mode == kTileModeThin1D);

    CHECK(compute_layout(tiled(100, 60, 0, DXGI_FORMAT_R8G8B8A8_UNORM), layout.get()) == S_OK);
    const PlaneLayout& chain = layout->plane[0];
    CHECK(layout->size_bytes == 44288 && layout->mip_levels == 7);
    CHECK(chain.mips[0].padded_width_elements == 128 && chain.mips[0].padded_height_elements == 64);
    CHECK(chain.mips[1].offset_bytes == 32768 && chain.mips[3].offset_bytes == 43008 && chain.mips[6].offset_bytes == 44032);
    CHECK(chain.mips[4].width_elements == 6 && chain.mips[4].height_elements == 3 && chain.mips[4].size_bytes == 256);

    CHECK(compute_layout(tiled(1, 1, 1, DXGI_FORMAT_A8_UNORM), layout.get()) == S_OK);
    CHECK(layout->plane[0].mips[0].padded_width_elements == 32 && layout->plane[0].mips[0].padded_height_elements == 8);
    CHECK(compute_layout(tiled(6, 10, 1, DXGI_FORMAT_A8_UNORM), layout.get()) == S_OK);
    CHECK(layout->plane[0].mips[0].padded_width_elements == 16 && layout->plane[0].mips[0].padded_height_elements == 16);

    CHECK(compute_layout(tiled(1, 17, 0, DXGI_FORMAT_R8G8B8A8_UNORM), layout.get()) == S_OK);
    CHECK(layout->size_bytes == 2048 && layout->plane[0].mips[1].padded_height_elements == 8);
    CHECK(compute_layout(tiled(17, 1, 0, DXGI_FORMAT_R8G8B8A8_UNORM), layout.get()) == S_OK);
    CHECK(layout->size_bytes == 2304 && layout->plane[0].mips[1].padded_width_elements == 16);

    CHECK(compute_layout(tiled(1, 2, 0, DXGI_FORMAT_R8G8B8A8_UNORM, 3), layout.get()) == S_OK);
    CHECK(layout->size_bytes == 2048 && layout->plane[0].mips[0].padded_depth_or_array_size == 4);
    CHECK(layout->plane[0].mips[0].depth_or_array_size == 3);

    CHECK(compute_layout(tiled(300, 200, 0, DXGI_FORMAT_BC1_UNORM), layout.get()) == S_OK);
    CHECK(layout->size_bytes == 89600 && layout->plane[0].mips[0].pitch_pixels == 512);
    CHECK(layout->plane[0].mips[0].padded_width_elements == 128 && layout->plane[0].mips[0].width_elements == 75);

    Texture2DDesc unknown = tiled(16, 16, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    unknown.tile_mode = 14;
    CHECK(compute_layout(unknown, layout.get()) == E_INVALIDARG);
}

TEST(xg_tiled_elements_follow_the_console_order) {
    CHECK(tiled_element_offset(0, 0, 64, 4) == 0);
    CHECK(tiled_element_offset(1, 0, 64, 4) == 0x4 && tiled_element_offset(0, 1, 64, 4) == 0x8);
    CHECK(tiled_element_offset(2, 0, 64, 4) == 0x10 && tiled_element_offset(4, 0, 64, 4) == 0x40);
    CHECK(tiled_element_offset(7, 7, 64, 4) == 0xFC && tiled_element_offset(8, 0, 64, 4) == 0x100);
    CHECK(tiled_element_offset(0, 8, 64, 4) == 0x800 && tiled_element_offset(9, 8, 64, 4) == 0x904);
    CHECK(tiled_element_offset(8, 0, 2048, 4) == 0x100 && tiled_element_offset(0, 8, 2048, 4) == 0x10000);
    CHECK(tiled_element_offset(1, 1, 64, 1) == 0x3 && tiled_element_offset(4, 4, 64, 1) == 0x30);
    CHECK(tiled_element_offset(1, 0, 64, 8) == 0x8 && tiled_element_offset(0, 2, 64, 8) == 0x40);
    CHECK(tiled_element_offset(3, 3, 64, 16) == 0xF0);
}

TEST(xg_detiling_restores_rows) {
    constexpr std::uint32_t kPitch = 16;
    constexpr std::uint32_t kWidth = 12;
    constexpr std::uint32_t kHeight = 10;
    std::vector<unsigned char> tiled_bytes(kPitch * 16 * 2);
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint64_t at = tiled_element_offset(x, y, kPitch, 2);
            tiled_bytes[at] = static_cast<unsigned char>(x);
            tiled_bytes[at + 1] = static_cast<unsigned char>(y);
        }
    std::vector<unsigned char> rows(64 * kHeight);
    detile(tiled_bytes.data(), kPitch, kWidth, kHeight, 2, rows.data(), 64);
    bool ok = true;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x)
            ok = ok && rows[y * 64 + x * 2] == x && rows[y * 64 + x * 2 + 1] == y;
    CHECK(ok);
}

extern "C" std::uint32_t g6shim_XGComputeOptimalTileMode(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                         std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

TEST(xg_finds_the_console_library_next_to_itself) {
    CHECK(console_library_path(L"C:/game/xg_x.dll") == L"C:/game/console_xg_x.dll");
    CHECK(console_library_path(L"C:\\game\\sub\\d3d12_x.dll") == L"C:\\game\\sub\\console_xg_x.dll");
    CHECK(console_library_path(L"xg_x.dll").empty());
}

TEST(xg_uses_its_own_layouts_without_the_console_library) {
    CHECK(console_library() == nullptr);
    CHECK(console_export("XGCreateTexture2DComputer") == nullptr);
    CHECK(g6shim_XGComputeOptimalTileMode(kResourceDimensionTexture2D, 28, 64, 64, 1, 1, 8, 0) == kTileModeThin1D);
}
