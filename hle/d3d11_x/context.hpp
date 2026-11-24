// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "proxy.hpp"

#include <d3d11.h>

#include <g6shim/log.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g6shim::d3d {

inline constexpr int kTracedDraws = 3;

ID3D11DeviceContext* real_context(void* proxy);

void apply_topology(void* self);

void count_draw();
unsigned take_draws();

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE DrawWithTopology(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                                           std::uintptr_t d, std::uintptr_t e) {
    apply_topology(self);
    count_draw();
    static std::atomic<int> calls{0};
    if (calls++ < kTracedDraws)
        G6SHIM_TRACE("context slot %zu (a draw): %llx %llx %llx %llx %llx", Slot, static_cast<unsigned long long>(a),
                     static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
                     static_cast<unsigned long long>(d), static_cast<unsigned long long>(e));
    return forward5(Slot, self, a, b, c, d, e);
}

void STDMETHODCALLTYPE ResolveSubresource(void* self, ID3D11Resource* dst, UINT dst_sub, ID3D11Resource* src,
                                          UINT src_sub, DXGI_FORMAT format);

void* make_context_proxy(ID3D11DeviceContext* context);

}  // namespace g6shim::d3d
