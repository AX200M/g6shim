// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "placement.hpp"

#include "console_layout.hpp"
#include "proxy.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <map>
#include <mutex>

namespace g6shim::d3d {

namespace {

constexpr int kLoggedCreations = 8;

std::mutex g_mutex;
std::map<const void*, Placement> g_placements;

}  // namespace

bool placement_of(const void* buffer, Placement& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_placements.find(buffer);
    if (it == g_placements.end()) return false;
    out = it->second;
    return true;
}

void forget_placement(const void* buffer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_placements.erase(buffer);
}

HRESULT STDMETHODCALLTYPE CreatePlacementBuffer(void* self, const D3D11_BUFFER_DESC* in, void* address,
                                                ID3D11Buffer** buffer) {
    if (!in || !address) return E_INVALIDARG;
    auto* device = static_cast<ID3D11Device*>(real_object(self));
    D3D11_BUFFER_DESC desc = *in;
    if (desc.Usage == D3D11_USAGE_DYNAMIC && desc.CPUAccessFlags == 0) desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (desc.Usage == D3D11_USAGE_DEFAULT) desc.CPUAccessFlags = 0;
    const D3D11_SUBRESOURCE_DATA data{address, 0, 0};
    const HRESULT hr = device->CreateBuffer(&desc, &data, buffer);
    if (SUCCEEDED(hr) && buffer && *buffer) {
        use_console_layout(*buffer, Layout::Resource);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_placements[*buffer] = {static_cast<const std::uint8_t*>(address), desc.ByteWidth};
    }
    static std::atomic<int> calls{0};
    const bool early = calls++ < kLoggedCreations;
    if (FAILED(hr) || (early && trace_enabled()))
        log(FAILED(hr) ? Level::Warn : Level::Trace,
            "CreatePlacementBuffer: %u bytes, usage %d, bind 0x%X, cpu 0x%X, misc 0x%X, stride %u at %p -> 0x%08lX",
            desc.ByteWidth, static_cast<int>(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags,
            desc.StructureByteStride, address, static_cast<unsigned long>(hr));
    return hr;
}

}  // namespace g6shim::d3d
