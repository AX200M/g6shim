// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "proxy.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g6shim::d3d {

inline constexpr int kTracedCalls = 8;

const char* object_kind(std::uintptr_t value);
std::uintptr_t peek(std::uintptr_t value);

template <std::size_t Offset, std::size_t Slot>
HRESULT STDMETHODCALLTYPE TraceFastCall(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                                        std::uintptr_t d, std::uintptr_t e) {
    static std::atomic<int> calls{0};
    if (calls++ < kTracedCalls)
        G6SHIM_TRACE("fast +0x%zX: a=%llx (%s, *a %s) b=%llx (%s, *b %s) c=%llx d=%llx e=%llx", Offset,
                     static_cast<unsigned long long>(a), object_kind(a), object_kind(peek(a)),
                     static_cast<unsigned long long>(b), object_kind(b), object_kind(peek(b)),
                     static_cast<unsigned long long>(c), static_cast<unsigned long long>(d),
                     static_cast<unsigned long long>(e));
    return forward5(Slot, self, a, b, c, d, e);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE TraceContextCall(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                                           std::uintptr_t d, std::uintptr_t e) {
    static std::atomic<int> calls{0};
    if (calls++ < kTracedCalls)
        G6SHIM_TRACE("context vtable slot %zu: a=%llx (%s) b=%llx c=%llx", Slot, static_cast<unsigned long long>(a),
                     object_kind(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
    return forward5(Slot, self, a, b, c, d, e);
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE CheckedDeviceCall(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                                            std::uintptr_t d, std::uintptr_t e) {
    const HRESULT hr = forward5(Slot, self, a, b, c, d, e);
    static std::atomic<int> calls{0};
    if (FAILED(hr) || (calls++ < kTracedCalls && trace_enabled()))
        log(FAILED(hr) ? Level::Warn : Level::Trace, "device slot %zu: %llx %llx %llx %llx -> 0x%08lX", Slot,
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
            static_cast<unsigned long long>(d), static_cast<unsigned long>(hr));
    return hr;
}

template <std::size_t Slot>
const void* checked_device_slot() {
    return reinterpret_cast<const void*>(&CheckedDeviceCall<Slot>);
}

template <std::size_t Slot>
const void* traced_context_slot() {
    return trace_enabled() ? reinterpret_cast<const void*>(&TraceContextCall<Slot>) : forward_thunk(Slot);
}

template <std::size_t Offset, std::size_t Slot>
const void* traced_fast_slot() {
    return trace_enabled() ? reinterpret_cast<const void*>(&TraceFastCall<Offset, Slot>) : forward_thunk(Slot);
}

}  // namespace g6shim::d3d
