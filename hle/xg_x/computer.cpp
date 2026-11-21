// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "computer.hpp"

#include <g6shim/log.hpp>

#include <intrin.h>

#include <array>
#include <atomic>
#include <new>
#include <utility>

namespace g6shim::xg {

namespace {

constexpr int kLoggedCalls = 64;

ULONG STDMETHODCALLTYPE computer_add_ref(TextureComputer* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE computer_release(TextureComputer* self) {
    const ULONG left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

HRESULT STDMETHODCALLTYPE computer_resource_layout(TextureComputer* self, ResourceLayout* layout) {
    if (!layout) return E_POINTER;
    *layout = self->layout;
    return S_OK;
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE computer_probe(TextureComputer*, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < kLoggedCalls)
        G6SHIM_WARN("XG texture computer: slot %zu not implemented, called from %p, args 0x%llX 0x%llX 0x%llX -> E_NOTIMPL",
                    Slot, _ReturnAddress(), static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                    static_cast<unsigned long long>(c));
    return E_NOTIMPL;
}

template <std::size_t... I>
constexpr std::array<const void*, kComputerSlots> computer_table(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&computer_probe<I>)...};
}

const void* const* table() {
    static const std::array<const void*, kComputerSlots> slots = [] {
        auto t = computer_table(std::make_index_sequence<kComputerSlots>{});
        t[computer_slot::kAddRef] = reinterpret_cast<const void*>(&computer_add_ref);
        t[computer_slot::kRelease] = reinterpret_cast<const void*>(&computer_release);
        t[computer_slot::kGetResourceLayout] = reinterpret_cast<const void*>(&computer_resource_layout);
        return t;
    }();
    return slots.data();
}

}  // namespace

HRESULT create_texture_2d_computer(const Texture2DDesc* desc, TextureComputer** out) {
    if (!desc || !out) return E_POINTER;
    *out = nullptr;
    auto* computer = new (std::nothrow) TextureComputer{};
    if (!computer) return E_OUTOFMEMORY;
    const HRESULT hr = compute_layout(*desc, &computer->layout);
    if (FAILED(hr)) {
        delete computer;
        return hr;
    }
    computer->table = table();
    computer->refs.store(1, std::memory_order_relaxed);
    *out = computer;
    return S_OK;
}

}  // namespace g6shim::xg

using namespace g6shim::xg;

extern "C" std::uint32_t g6shim_XGComputeOptimalTileMode(std::uint32_t dimension, std::uint32_t, std::uint32_t,
                                                         std::uint32_t, std::uint32_t, std::uint32_t samples,
                                                         std::uint32_t, std::uint32_t) {
    return optimal_tile_mode(dimension, samples);
}

extern "C" HRESULT g6shim_XGCreateTexture2DComputer(const Texture2DDesc* desc, TextureComputer** out) {
    const HRESULT hr = create_texture_2d_computer(desc, out);
    static std::atomic<int> logged{0};
    if (desc && (FAILED(hr) || logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCalls))
        G6SHIM_INFO("XGCreateTexture2DComputer(%ux%u, %u mips, %u slices, format %u, tile mode %u) -> 0x%08lX",
                    desc->width, desc->height, desc->mip_levels, desc->array_size, desc->format, desc->tile_mode, hr);
    return hr;
}
