// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "view_desc.hpp"

using namespace g6shim::d3d;

TEST(view_desc_depth_formats_become_readable) {
    CHECK(readable_format(DXGI_FORMAT_D16_UNORM) == DXGI_FORMAT_R16_UNORM);
    CHECK(readable_format(DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_R32_FLOAT);
    CHECK(readable_format(DXGI_FORMAT_D24_UNORM_S8_UINT) == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
    CHECK(readable_format(DXGI_FORMAT_D32_FLOAT_S8X24_UINT) == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    CHECK(readable_format(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_UNORM);
}

TEST(view_desc_shader_resource_views_lose_multisampling) {
    D3D11_SHADER_RESOURCE_VIEW_DESC in{};
    in.Format = DXGI_FORMAT_D16_UNORM;
    in.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    ViewDesc d{};
    const auto* out = static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC*>(pc_view_desc(ViewKind::ShaderResource, &in, d));
    CHECK(out == &d.srv);
    CHECK(out->Format == DXGI_FORMAT_R16_UNORM);
    CHECK(out->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
    CHECK(out->Texture2D.MostDetailedMip == 0 && out->Texture2D.MipLevels == 1);

    in.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    in.Texture2DMSArray = {2, 3};
    out = static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC*>(pc_view_desc(ViewKind::ShaderResource, &in, d));
    CHECK(out->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
    CHECK(out->Texture2DArray.FirstArraySlice == 2 && out->Texture2DArray.ArraySize == 3);
    CHECK(out->Texture2DArray.MipLevels == 1);
}

TEST(view_desc_render_target_and_depth_views) {
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    rtv.Texture2DMSArray = {1, 4};
    ViewDesc d{};
    const auto* r = static_cast<const D3D11_RENDER_TARGET_VIEW_DESC*>(pc_view_desc(ViewKind::RenderTarget, &rtv, d));
    CHECK(r->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY);
    CHECK(r->Texture2DArray.FirstArraySlice == 1 && r->Texture2DArray.ArraySize == 4 && r->Texture2DArray.MipSlice == 0);

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D16_UNORM;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    const auto* s = static_cast<const D3D11_DEPTH_STENCIL_VIEW_DESC*>(pc_view_desc(ViewKind::DepthStencil, &dsv, d));
    CHECK(s->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && s->Format == DXGI_FORMAT_D16_UNORM);
}

TEST(view_desc_passes_through_other_views) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    ViewDesc d{};
    CHECK(pc_view_desc(ViewKind::UnorderedAccess, &uav, d) == &uav);
    CHECK(pc_view_desc(ViewKind::ShaderResource, nullptr, d) == nullptr);
    D3D11_SHADER_RESOURCE_VIEW_DESC plain{};
    plain.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    plain.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    plain.Texture2D = {2, 5};
    const auto* out = static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC*>(pc_view_desc(ViewKind::ShaderResource, &plain, d));
    CHECK(out->Texture2D.MostDetailedMip == 2 && out->Texture2D.MipLevels == 5);
}
