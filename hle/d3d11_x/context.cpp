// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "context.hpp"

#include "fast_table.hpp"
#include "slots.hpp"
#include "topology.hpp"
#include "trace.hpp"

#include <mutex>
#include <set>

namespace g6shim::d3d {

namespace {

std::atomic<unsigned> g_draws{0};

}  // namespace

ID3D11DeviceContext* real_context(void* proxy) { return static_cast<ID3D11DeviceContext*>(real_object(proxy)); }

void count_draw() { ++g_draws; }

unsigned take_draws() { return g_draws.exchange(0); }

void apply_topology(void* self) {
    static thread_local std::uint64_t applied = 0;
    const std::uint64_t value = *reinterpret_cast<const std::uint64_t*>(static_cast<const char*>(self) + kTopologyField);
    if (value == applied) return;
    applied = value;
    const D3D11_PRIMITIVE_TOPOLOGY topology = decode_topology(value);
    if (trace_enabled()) {
        static std::mutex mutex;
        static std::set<std::uint64_t> seen;
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.insert(value).second)
            G6SHIM_TRACE("context topology field 0x%llX -> D3D11 topology %d", static_cast<unsigned long long>(value),
                         static_cast<int>(topology));
    }
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) real_context(self)->IASetPrimitiveTopology(topology);
}

void STDMETHODCALLTYPE ResolveSubresource(void* self, ID3D11Resource* dst, UINT dst_sub, ID3D11Resource* src,
                                          UINT src_sub, DXGI_FORMAT format) {
    auto* c = real_context(self);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (src) src->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(src)->GetDesc(&desc);
        if (desc.SampleDesc.Count == 1) {
            c->CopySubresourceRegion(dst, dst_sub, 0, 0, 0, src, src_sub, nullptr);
            return;
        }
    }
    c->ResolveSubresource(dst, dst_sub, src, src_sub, format);
}

void* make_context_proxy(ID3D11DeviceContext* context) {
    using namespace context_slot;
    return make_proxy(context, kPcContext1Slots, "ID3D11DeviceContextX",
                      {{kPSSetShader, traced_context_slot<kPSSetShader>()},
                       {kVSSetShader, traced_context_slot<kVSSetShader>()},
                       {kDrawIndexed, reinterpret_cast<const void*>(&DrawWithTopology<kDrawIndexed>)},
                       {kDraw, reinterpret_cast<const void*>(&DrawWithTopology<kDraw>)},
                       {kIASetInputLayout, traced_context_slot<kIASetInputLayout>()},
                       {kDrawIndexedInstanced, reinterpret_cast<const void*>(&DrawWithTopology<kDrawIndexedInstanced>)},
                       {kDrawInstanced, reinterpret_cast<const void*>(&DrawWithTopology<kDrawInstanced>)},
                       {kIASetPrimitiveTopology, traced_context_slot<kIASetPrimitiveTopology>()},
                       {kDrawAuto, reinterpret_cast<const void*>(&DrawWithTopology<kDrawAuto>)},
                       {kDrawIndexedInstancedIndirect,
                        reinterpret_cast<const void*>(&DrawWithTopology<kDrawIndexedInstancedIndirect>)},
                       {kDrawInstancedIndirect, reinterpret_cast<const void*>(&DrawWithTopology<kDrawInstancedIndirect>)},
                       {kResolveSubresource, reinterpret_cast<const void*>(&ResolveSubresource)}},
                      context_fields());
}

}  // namespace g6shim::d3d
