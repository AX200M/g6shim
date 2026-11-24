// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>

namespace g6shim::d3d {

enum class ViewKind { ShaderResource, UnorderedAccess, RenderTarget, DepthStencil };

union ViewDesc {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv;
    D3D11_RENDER_TARGET_VIEW_DESC rtv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
};

inline DXGI_FORMAT readable_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return f;
    }
}

inline void single_sample(D3D11_SHADER_RESOURCE_VIEW_DESC& d) {
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS) {
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D = {0, 1};
    } else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY) {
        const auto ms = d.Texture2DMSArray;
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {0, 1, ms.FirstArraySlice, ms.ArraySize};
    }
}

inline void single_sample(D3D11_RENDER_TARGET_VIEW_DESC& d) {
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) {
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D = {0};
    } else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
        const auto ms = d.Texture2DMSArray;
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {0, ms.FirstArraySlice, ms.ArraySize};
    }
}

inline void single_sample(D3D11_DEPTH_STENCIL_VIEW_DESC& d) {
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMS) {
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D = {0};
    } else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) {
        const auto ms = d.Texture2DMSArray;
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {0, ms.FirstArraySlice, ms.ArraySize};
    }
}

inline const void* pc_view_desc(ViewKind kind, const void* in, ViewDesc& d) {
    if (!in) return in;
    switch (kind) {
        case ViewKind::ShaderResource:
            d.srv = *static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC*>(in);
            d.srv.Format = readable_format(d.srv.Format);
            single_sample(d.srv);
            return &d;
        case ViewKind::RenderTarget:
            d.rtv = *static_cast<const D3D11_RENDER_TARGET_VIEW_DESC*>(in);
            single_sample(d.rtv);
            return &d;
        case ViewKind::DepthStencil:
            d.dsv = *static_cast<const D3D11_DEPTH_STENCIL_VIEW_DESC*>(in);
            single_sample(d.dsv);
            return &d;
        default: return in;
    }
}

}  // namespace g6shim::d3d
