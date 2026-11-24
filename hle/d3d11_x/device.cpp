// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <d3d11.h>

#include <g6shim/log.hpp>

#include "console_layout.hpp"
#include "context.hpp"
#include "dxgi.hpp"
#include "placement.hpp"
#include "proxy.hpp"
#include "slots.hpp"
#include "trace.hpp"
#include "vtable_patch.hpp"

#include <cstdint>

namespace g6shim::d3d {

namespace {

constexpr GUID kDeviceX = {0x177700F9, 0x876A, 0x4436, {0xB3, 0x68, 0x36, 0xA6, 0x04, 0xF8, 0x2C, 0xEF}};
constexpr GUID kContextX = {0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};
constexpr UINT kPcMiscFlags = 0xFFFF;
constexpr char kDebugVariable[] = "G6SHIM_D3D_DEBUG";
constexpr std::size_t kLoggedParameterWords = 8;

constexpr UINT kPcCreateFlags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_DEBUG |
                                D3D11_CREATE_DEVICE_SWITCH_TO_REF |
                                D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS |
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUGGABLE |
                                D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY |
                                D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
QueryInterfaceFn g_device_qi = nullptr;
QueryInterfaceFn g_context_qi = nullptr;

void* g_device_proxy = nullptr;
void* g_context_proxy = nullptr;

void log_qi(const char* what, REFIID iid, HRESULT hr) {
    G6SHIM_TRACE("%s QueryInterface {%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X} -> 0x%08lX", what,
                 iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7], static_cast<unsigned long>(hr));
}

void* share(void* proxy) {
    real_object(proxy)->AddRef();
    return proxy;
}

HRESULT STDMETHODCALLTYPE DeviceQueryInterface(IUnknown* self, REFIID iid, void** out) {
    if (iid == kDeviceX && out && g_device_proxy) {
        *out = share(g_device_proxy);
        return S_OK;
    }
    const HRESULT hr = g_device_qi(self, iid, out);
    log_qi("ID3D11Device", iid, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE ContextQueryInterface(IUnknown* self, REFIID iid, void** out) {
    if (iid == kContextX && out && g_context_proxy) {
        *out = share(g_context_proxy);
        return S_OK;
    }
    const HRESULT hr = g_context_qi(self, iid, out);
    log_qi("ID3D11DeviceContext", iid, hr);
    return hr;
}

void STDMETHODCALLTYPE GetImmediateContext(void*, void** context) {
    if (context) *context = g_context_proxy ? share(g_context_proxy) : nullptr;
}

HRESULT STDMETHODCALLTYPE CreateTexture2D(void* self, const D3D11_TEXTURE2D_DESC* in, const D3D11_SUBRESOURCE_DATA* data,
                                          ID3D11Texture2D** texture) {
    auto* device = static_cast<ID3D11Device*>(real_object(self));
    D3D11_TEXTURE2D_DESC fixed = in ? *in : D3D11_TEXTURE2D_DESC{};
    if (in && fixed.Usage == D3D11_USAGE_DYNAMIC && fixed.CPUAccessFlags == 0) fixed.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    fixed.MiscFlags &= kPcMiscFlags;
    fixed.SampleDesc = {1, 0};
    const D3D11_TEXTURE2D_DESC* desc = in ? &fixed : nullptr;
    const HRESULT hr = device->CreateTexture2D(desc, data, texture);
    if (SUCCEEDED(hr) && texture) use_console_layout(*texture, Layout::Resource);
    if (FAILED(hr) && desc)
        G6SHIM_WARN("CreateTexture2D %ux%u mips %u array %u format %d samples %u/%u usage %d bind 0x%X cpu 0x%X "
                    "misc 0x%X, data %p -> 0x%08lX",
                    desc->Width, desc->Height, desc->MipLevels, desc->ArraySize, static_cast<int>(desc->Format),
                    desc->SampleDesc.Count, desc->SampleDesc.Quality, static_cast<int>(desc->Usage), desc->BindFlags,
                    desc->CPUAccessFlags, desc->MiscFlags, static_cast<const void*>(data), static_cast<unsigned long>(hr));
    return hr;
}

template <class F>
const void* fn(F f) {
    return reinterpret_cast<const void*>(f);
}

void make_proxies(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (g_device_proxy || !context) return;
    g_context_proxy = make_context_proxy(context);
    using namespace device_slot;
    g_device_proxy = make_proxy(
        device, kPcDevice1Slots, "ID3D11DeviceX",
        {{kCreateBuffer, fn(&CreateWithConsoleLayout<kCreateBuffer, Layout::Resource>)},
         {kCreateTexture1D, fn(&CreateWithConsoleLayout<kCreateTexture1D, Layout::Resource>)},
         {kCreateTexture2D, fn(&CreateTexture2D)},
         {kCreateTexture3D, fn(&CreateWithConsoleLayout<kCreateTexture3D, Layout::Resource>)},
         {kCreateShaderResourceView, fn(&CreateWithConsoleLayout<kCreateShaderResourceView, Layout::View>)},
         {kCreateUnorderedAccessView, fn(&CreateWithConsoleLayout<kCreateUnorderedAccessView, Layout::View>)},
         {kCreateRenderTargetView, fn(&CreateWithConsoleLayout<kCreateRenderTargetView, Layout::View>)},
         {kCreateDepthStencilView, fn(&CreateWithConsoleLayout<kCreateDepthStencilView, Layout::View>)},
         {kCreateInputLayout, checked_device_slot<kCreateInputLayout>()},
         {kCreateVertexShader, checked_device_slot<kCreateVertexShader>()},
         {kCreatePixelShader, checked_device_slot<kCreatePixelShader>()},
         {kGetImmediateContext, fn(&GetImmediateContext)},
         {kGetImmediateContext1, fn(&GetImmediateContext)},
         {kCreatePlacementBuffer, fn(&CreatePlacementBuffer)}});
}

void hook_query_interface(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (auto original = patch_vtable(device, device_slot::kQueryInterface, &DeviceQueryInterface)) g_device_qi = original;
    if (context)
        if (auto original = patch_vtable(context, context_slot::kQueryInterface, &ContextQueryInterface))
            g_context_qi = original;
}

bool debug_layer_requested() {
    char debug[8] = "";
    return GetEnvironmentVariableA(kDebugVariable, debug, sizeof debug) && debug[0] == '1';
}

}  // namespace

}  // namespace g6shim::d3d

extern "C" HRESULT g6shim_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk_version,
                                            ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                            ID3D11DeviceContext** context) {
    using namespace g6shim::d3d;
    G6SHIM_INFO("D3D11CreateDevice: driver %d, flags 0x%X (PC keeps 0x%X), %u feature level(s) (first 0x%X), "
                "SDK version %u",
                static_cast<int>(driver), flags, flags & kPcCreateFlags, level_count,
                levels && level_count ? static_cast<unsigned>(levels[0]) : 0u, sdk_version);
    UINT pc_flags = flags & kPcCreateFlags;
    if (debug_layer_requested()) {
        pc_flags |= D3D11_CREATE_DEVICE_DEBUG;
        G6SHIM_INFO("D3D11CreateDevice: Direct3D debug layer on (%s=1)", kDebugVariable);
    }
    const HRESULT hr = D3D11CreateDevice(adapter, driver, software, pc_flags, levels, level_count, D3D11_SDK_VERSION,
                                         device, level, context);
    G6SHIM_INFO("D3D11CreateDevice -> 0x%08lX, feature level 0x%X", static_cast<unsigned long>(hr),
                level ? static_cast<unsigned>(*level) : 0u);
    if (SUCCEEDED(hr) && device && *device && context && *context) {
        hook_dxgi_factory(*device);
        hook_query_interface(*device, *context);
        make_proxies(*device, *context);
        if (g_device_proxy && g_context_proxy) {
            *device = static_cast<ID3D11Device*>(g_device_proxy);
            *context = static_cast<ID3D11DeviceContext*>(g_context_proxy);
            G6SHIM_INFO("D3D11CreateDevice: returning the ID3D11DeviceX / ID3D11DeviceContextX proxies");
        }
    }
    return hr;
}

extern "C" HRESULT g6shim_D3D11XCreateDeviceX(const std::uint32_t* parameters, ID3D11Device** device,
                                              ID3D11DeviceContext** context) {
    if (parameters)
        G6SHIM_INFO("D3D11XCreateDeviceX: parameters %08X %08X %08X %08X %08X %08X %08X %08X", parameters[0],
                    parameters[1], parameters[2], parameters[3], parameters[4], parameters[5], parameters[6],
                    parameters[7]);
    static_assert(g6shim::d3d::kLoggedParameterWords == 8);
    if (!device) return E_POINTER;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return g6shim_D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, device,
                                    nullptr, context);
}
