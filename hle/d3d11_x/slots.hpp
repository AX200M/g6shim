// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>

namespace g6shim::d3d {

namespace device_slot {
inline constexpr std::size_t kQueryInterface = 0;
inline constexpr std::size_t kCreateBuffer = 3;
inline constexpr std::size_t kCreateTexture1D = 4;
inline constexpr std::size_t kCreateTexture2D = 5;
inline constexpr std::size_t kCreateTexture3D = 6;
inline constexpr std::size_t kCreateShaderResourceView = 7;
inline constexpr std::size_t kCreateUnorderedAccessView = 8;
inline constexpr std::size_t kCreateRenderTargetView = 9;
inline constexpr std::size_t kCreateDepthStencilView = 10;
inline constexpr std::size_t kCreateInputLayout = 11;
inline constexpr std::size_t kCreateVertexShader = 12;
inline constexpr std::size_t kCreatePixelShader = 15;
inline constexpr std::size_t kGetImmediateContext = 40;
inline constexpr std::size_t kGetImmediateContext1 = 43;
inline constexpr std::size_t kPcDevice1Slots = 50;
inline constexpr std::size_t kCreatePlacementBuffer = 62;
}  // namespace device_slot

namespace context_slot {
inline constexpr std::size_t kQueryInterface = 0;
inline constexpr std::size_t kFirstContextMethod = 7;
inline constexpr std::size_t kVSSetConstantBuffers = 7;
inline constexpr std::size_t kPSSetShaderResources = 8;
inline constexpr std::size_t kPSSetShader = 9;
inline constexpr std::size_t kPSSetSamplers = 10;
inline constexpr std::size_t kVSSetShader = 11;
inline constexpr std::size_t kDrawIndexed = 12;
inline constexpr std::size_t kDraw = 13;
inline constexpr std::size_t kPSSetConstantBuffers = 16;
inline constexpr std::size_t kIASetInputLayout = 17;
inline constexpr std::size_t kIASetVertexBuffers = 18;
inline constexpr std::size_t kIASetIndexBuffer = 19;
inline constexpr std::size_t kDrawIndexedInstanced = 20;
inline constexpr std::size_t kDrawInstanced = 21;
inline constexpr std::size_t kGSSetShader = 23;
inline constexpr std::size_t kIASetPrimitiveTopology = 24;
inline constexpr std::size_t kOMSetBlendState = 35;
inline constexpr std::size_t kOMSetDepthStencilState = 36;
inline constexpr std::size_t kDrawAuto = 38;
inline constexpr std::size_t kDrawIndexedInstancedIndirect = 39;
inline constexpr std::size_t kDrawInstancedIndirect = 40;
inline constexpr std::size_t kRSSetState = 43;
inline constexpr std::size_t kClearRenderTargetView = 50;
inline constexpr std::size_t kResolveSubresource = 57;
inline constexpr std::size_t kPcContext1Slots = 144;
}  // namespace context_slot

namespace dxgi_slot {
inline constexpr std::size_t kPresent = 8;
inline constexpr std::size_t kCreateSwapChain = 10;
inline constexpr std::size_t kCreateSwapChainForCoreWindow = 16;
inline constexpr std::size_t kPresent1 = 22;
}  // namespace dxgi_slot

}  // namespace g6shim::d3d
