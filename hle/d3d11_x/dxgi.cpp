// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dxgi.hpp"

#include "capture.hpp"
#include "context.hpp"
#include "slots.hpp"
#include "vtable_patch.hpp"

#include <windows.h>
#include <dxgi1_2.h>

#include <g6shim/log.hpp>
#include <g6shim/window.hpp>

#include <atomic>
#include <cstdint>

namespace g6shim::d3d {

namespace {

constexpr UINT kPcSwapChainFlags = 0x1FFF;
constexpr UINT kPcPresentFlags = 0xFFFF;
constexpr UINT kMinFlipBuffers = 2;
constexpr unsigned kPresentLogInterval = 600;
constexpr int kLoggedPresentArrayCalls = 3;

using CreateForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                          const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

CreateForCoreWindowFn g_original_for_core_window = nullptr;
CreateSwapChainFn g_original_create_swap_chain = nullptr;
PresentFn g_original_present = nullptr;
Present1Fn g_original_present1 = nullptr;

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* self, UINT sync_interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS* parameters) {
    static const DXGI_PRESENT_PARAMETERS none{};
    static std::atomic<unsigned> frames{0};
    const unsigned n = frames++;
    if (n == 0) G6SHIM_INFO("first Present1 (sync interval %u, flags 0x%X)", sync_interval, flags);
    capture_if_requested(self, n);
    return g_original_present1(self, sync_interval, flags, parameters ? parameters : &none);
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    static std::atomic<unsigned> frames{0};
    const unsigned n = frames++;
    capture_if_requested(self, n);
    const HRESULT hr = g_original_present(self, sync_interval, flags & kPcPresentFlags);
    static unsigned last = 0;
    if (n == 0 || n % kPresentLogInterval == 0 || FAILED(hr)) {
        const unsigned draws = take_draws();
        G6SHIM_INFO("Present #%u (sync interval %u, flags 0x%X) -> 0x%08lX, %u draws per frame", n, sync_interval,
                    flags, static_cast<unsigned long>(hr), n > last ? draws / (n - last) : draws);
        last = n;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_to,
                                                       IDXGISwapChain1** swap_chain) {
    const HWND hwnd = static_cast<HWND>(main_window());
    if (!hwnd || !desc) return g_original_for_core_window(factory, device, window, desc, restrict_to, swap_chain);
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    G6SHIM_INFO("CreateSwapChainForCoreWindow: %ux%u format %d, %u buffers, swap effect %d, scaling %d, alpha %d, "
                "flags 0x%X -> for the g6shim window",
                d.Width, d.Height, static_cast<int>(d.Format), d.BufferCount, static_cast<int>(d.SwapEffect),
                static_cast<int>(d.Scaling), static_cast<int>(d.AlphaMode), d.Flags);
    if (d.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH) d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    d.Flags &= kPcSwapChainFlags;
    const HRESULT hr = factory->CreateSwapChainForHwnd(device, hwnd, &d, nullptr, restrict_to, swap_chain);
    G6SHIM_INFO("CreateSwapChainForHwnd -> 0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && swap_chain && *swap_chain)
        if (auto original = patch_vtable(*swap_chain, dxgi_slot::kPresent1, &Present1)) g_original_present1 = original;
    return hr;
}

DXGI_FORMAT without_srgb(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}

HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                          IDXGISwapChain** swap_chain) {
    const HWND hwnd = static_cast<HWND>(main_window());
    if (!hwnd || !desc) return g_original_create_swap_chain(factory, device, desc, swap_chain);
    DXGI_SWAP_CHAIN_DESC d = *desc;
    G6SHIM_INFO("CreateSwapChain: %ux%u format %d, %u buffers, swap effect %d, window %p, windowed %d, flags 0x%X "
                "-> for the g6shim window",
                d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format), d.BufferCount,
                static_cast<int>(d.SwapEffect), static_cast<void*>(d.OutputWindow), d.Windowed, d.Flags);
    d.OutputWindow = hwnd;
    d.Windowed = TRUE;
    d.Flags &= kPcSwapChainFlags;
    if (d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (d.BufferCount < kMinFlipBuffers) d.BufferCount = kMinFlipBuffers;
    d.SampleDesc = {1, 0};
    d.BufferDesc.Format = without_srgb(d.BufferDesc.Format);
    const HRESULT hr = g_original_create_swap_chain(factory, device, &d, swap_chain);
    G6SHIM_INFO("CreateSwapChain (flip model, %u buffers) -> 0x%08lX", d.BufferCount, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && swap_chain && *swap_chain)
        if (auto original = patch_vtable(*swap_chain, dxgi_slot::kPresent, &Present)) g_original_present = original;
    return hr;
}

}  // namespace

void hook_dxgi_factory(ID3D11Device* device) {
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        if (auto original = patch_vtable(factory, dxgi_slot::kCreateSwapChain, &CreateSwapChain))
            g_original_create_swap_chain = original;
        if (auto original = patch_vtable(factory, dxgi_slot::kCreateSwapChainForCoreWindow, &CreateSwapChainForCoreWindow)) {
            g_original_for_core_window = original;
            G6SHIM_INFO("DXGI: CreateSwapChainForCoreWindow now targets the g6shim window");
        }
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgi_device) dxgi_device->Release();
}

}  // namespace g6shim::d3d

extern "C" HRESULT g6shim_DXGIXPresentArray(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d,
                                            std::uintptr_t e, std::uintptr_t f) {
    static std::atomic<int> calls{0};
    if (calls++ < g6shim::d3d::kLoggedPresentArrayCalls)
        G6SHIM_WARN("DXGIXPresentArray(%llx, %llx, %llx, %llx, %llx, %llx): not implemented yet",
                    static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                    static_cast<unsigned long long>(c), static_cast<unsigned long long>(d),
                    static_cast<unsigned long long>(e), static_cast<unsigned long long>(f));
    return S_OK;
}
