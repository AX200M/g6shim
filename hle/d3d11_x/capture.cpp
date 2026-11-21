// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "capture.hpp"

#include <windows.h>
#include <d3d11.h>

#include <g6shim/capture.hpp>
#include <g6shim/log.hpp>

namespace g6shim::d3d {

void capture_if_requested(IDXGISwapChain* swap_chain, unsigned frame) {
    if (!swap_chain || !capture_requested(frame)) return;
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
            save_captured_frame(frame, mapped.pData, desc.Width, desc.Height, mapped.RowPitch,
                                rgba ? PixelOrder::Rgba : PixelOrder::Bgra);
            context->Unmap(staging, 0);
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
