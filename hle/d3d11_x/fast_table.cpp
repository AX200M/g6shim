// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fast_table.hpp"

#include "context.hpp"
#include "packets.hpp"
#include "trace.hpp"

#include <d3d11.h>

#include <map>

namespace g6shim::d3d {

namespace {

using namespace context_slot;

constexpr UINT kIndexSize32 = 1;

void STDMETHODCALLTYPE FastPSSetShaderResources(void* self, ID3D11ShaderResourceView* const* views, UINT start_slot,
                                               UINT) {
    real_context(self)->PSSetShaderResources(start_slot, 1, views);
}

void STDMETHODCALLTYPE FastDrawIndexed(void* self, std::uint64_t start_and_count, INT base_vertex) {
    apply_topology(self);
    count_draw();
    real_context(self)->DrawIndexed(static_cast<UINT>(start_and_count >> 32), static_cast<UINT>(start_and_count),
                                    base_vertex);
}

void STDMETHODCALLTYPE FastVSSetShader(void* self, ID3D11VertexShader* shader) {
    real_context(self)->VSSetShader(shader, nullptr, 0);
}

void STDMETHODCALLTYPE FastPSSetShader(void* self, ID3D11PixelShader* shader) {
    real_context(self)->PSSetShader(shader, nullptr, 0);
}

void STDMETHODCALLTYPE FastGSSetShader(void* self, ID3D11GeometryShader* shader) {
    real_context(self)->GSSetShader(shader, nullptr, 0);
}

void STDMETHODCALLTYPE FastIASetInputLayout(void* self, ID3D11InputLayout* layout) {
    real_context(self)->IASetInputLayout(layout);
}

void STDMETHODCALLTYPE FastIASetIndexBuffer(void* self, UINT index_size, ID3D11Buffer* buffer, UINT offset) {
    DXGI_FORMAT format = DXGI_FORMAT_R16_UINT;
    if (index_size == kIndexSize32 || index_size == DXGI_FORMAT_R32_UINT) format = DXGI_FORMAT_R32_UINT;
    real_context(self)->IASetIndexBuffer(buffer, format, offset);
}

template <class F>
const void* fn(F f) {
    return reinterpret_cast<const void*>(f);
}

std::map<std::size_t, const void*> fast_overrides() {
    return {
        {kPSSetShaderResources, fn(&FastPSSetShaderResources)},
        {kDrawIndexed, fn(&FastDrawIndexed)},
        {kIASetIndexBuffer, fn(&FastIASetIndexBuffer)},
        {kPSSetShader, fn(&FastPSSetShader)},
        {kVSSetShader, fn(&FastVSSetShader)},
        {kIASetInputLayout, fn(&FastIASetInputLayout)},
        {kGSSetShader, fn(&FastGSSetShader)},
        {kDraw, fn(&DrawWithTopology<kDraw>)},
        {kDrawIndexedInstanced, fn(&DrawWithTopology<kDrawIndexedInstanced>)},
        {kDrawInstanced, fn(&DrawWithTopology<kDrawInstanced>)},
        {kDrawAuto, fn(&DrawWithTopology<kDrawAuto>)},
        {kDrawIndexedInstancedIndirect, fn(&DrawWithTopology<kDrawIndexedInstancedIndirect>)},
        {kDrawInstancedIndirect, fn(&DrawWithTopology<kDrawInstancedIndirect>)},
        {kResolveSubresource, fn(&ResolveSubresource)},
        {kVSSetConstantBuffers, traced_fast_slot<fast_offset(kVSSetConstantBuffers), kVSSetConstantBuffers>()},
        {kPSSetSamplers, traced_fast_slot<fast_offset(kPSSetSamplers), kPSSetSamplers>()},
        {kPSSetConstantBuffers, traced_fast_slot<fast_offset(kPSSetConstantBuffers), kPSSetConstantBuffers>()},
        {kIASetVertexBuffers, traced_fast_slot<fast_offset(kIASetVertexBuffers), kIASetVertexBuffers>()},
        {kOMSetBlendState, traced_fast_slot<fast_offset(kOMSetBlendState), kOMSetBlendState>()},
        {kOMSetDepthStencilState, traced_fast_slot<fast_offset(kOMSetDepthStencilState), kOMSetDepthStencilState>()},
        {kRSSetState, traced_fast_slot<fast_offset(kRSSetState), kRSSetState>()},
        {kClearRenderTargetView, traced_fast_slot<fast_offset(kClearRenderTargetView), kClearRenderTargetView>()},
    };
}

}  // namespace

std::vector<Override> context_fields() {
    std::vector<Override> fields;
    for (std::size_t offset = kProxyFirstField; offset < kFastTableOffset; offset += sizeof(void*))
        fields.emplace_back(offset, offset == kCallTableFlagField ? reinterpret_cast<const void*>(kCallTableFlags) : nullptr);
    const auto overrides = fast_overrides();
    for (std::size_t slot = kFirstContextMethod; slot < kPcContext1Slots; ++slot) {
        if (slot == kIASetPrimitiveTopology) continue;
        const auto it = overrides.find(slot);
        fields.emplace_back(fast_offset(slot), it != overrides.end() ? it->second : forward_thunk(slot));
    }
    fields.emplace_back(kPacketFunctionField, fn(&ContextPacket));
    return fields;
}

}  // namespace g6shim::d3d
