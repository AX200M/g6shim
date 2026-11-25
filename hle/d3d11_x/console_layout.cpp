// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console_layout.hpp"

#include "placement.hpp"
#include "proxy.hpp"
#include "slots.hpp"
#include "view_desc.hpp"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <g6shim/log.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <map>
#include <mutex>
#include <utility>

namespace g6shim::d3d {

namespace {

constexpr std::size_t kSlots = 16;
constexpr std::size_t kDeviceChildSlots = 7;
constexpr std::size_t kPcResourceSlots = 11;
constexpr std::size_t kPcViewSlots = 9;

struct Move {
    std::size_t console;
    std::size_t pc;
};

constexpr Move kResourceMoves[] = {{9, 7}, {10, 8}, {11, 9}, {13, 10}};
constexpr Move kViewMoves[] = {{9, 7}, {10, 8}};

struct Table {
    const void* entries[kSlots + 1];
    const void* pc[kSlots];
    const void* console[kSlots];
    const char* name;
};

const Table* table_of(const void* object) {
    const auto* vtable = *static_cast<const char* const*>(object);
    return reinterpret_cast<const Table*>(vtable - offsetof(Table, entries) - sizeof(void*));
}

bool is_pc_code(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
        return false;
    static std::mutex mutex;
    static std::map<HMODULE, bool> known;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = known.find(module);
    if (it != known.end()) return it->second;
    static const HMODULE self = [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&is_pc_code), &m);
        return m;
    }();
    wchar_t windows[MAX_PATH] = L"";
    wchar_t path[MAX_PATH] = L"";
    const UINT wn = GetWindowsDirectoryW(windows, MAX_PATH);
    const DWORD pn = GetModuleFileNameW(module, path, MAX_PATH);
    const bool pc = module == self || (wn > 0 && wn < MAX_PATH && pn > wn && _wcsnicmp(path, windows, wn) == 0 &&
                                       (path[wn] == L'\\' || path[wn] == L'/'));
    known[module] = pc;
    return pc;
}

template <std::size_t N>
HRESULT STDMETHODCALLTYPE ConsoleOnly(IUnknown* self, void* a, void* b, void* c) {
    G6SHIM_WARN("%s: console-only method, vtable slot %zu, called from %p, args %p %p %p -> E_NOTIMPL",
                table_of(self)->name, N, _ReturnAddress(), a, b, c);
    return E_NOTIMPL;
}

template <std::size_t N>
std::uintptr_t STDMETHODCALLTYPE Either(IUnknown* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    const Table* t = table_of(self);
    using Fn = std::uintptr_t(STDMETHODCALLTYPE*)(IUnknown*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    return reinterpret_cast<Fn>(is_pc_code(_ReturnAddress()) ? t->pc[N] : t->console[N])(self, a, b, c);
}

template <std::size_t... I>
std::array<const void*, kSlots> console_only(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&ConsoleOnly<I>)...};
}

template <std::size_t... I>
std::array<const void*, kSlots> either(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&Either<I>)...};
}

Table* build(const void* const* pc, Layout layout) {
    static const auto probes = console_only(std::make_index_sequence<kSlots>{});
    static const auto dispatch = either(std::make_index_sequence<kSlots>{});
    const bool resource = layout == Layout::Resource;
    const std::size_t pc_slots = resource ? kPcResourceSlots : kPcViewSlots;
    auto* t = new Table{};
    for (std::size_t s = 0; s < kSlots; ++s) {
        t->pc[s] = s < pc_slots ? pc[s] : nullptr;
        t->console[s] = s < kDeviceChildSlots ? pc[s] : probes[s];
    }
    if (resource)
        for (const Move& m : kResourceMoves) t->console[m.console] = pc[m.pc];
    else
        for (const Move& m : kViewMoves) t->console[m.console] = pc[m.pc];
    t->name = resource ? "ID3D11Resource (console layout)" : "ID3D11View (console layout)";
    t->entries[0] = pc[-1];
    for (std::size_t s = 0; s < kSlots; ++s)
        t->entries[1 + s] = !t->pc[s] || t->pc[s] == t->console[s] ? t->console[s] : dispatch[s];
    return t;
}

std::mutex g_mutex;
std::map<const void*, Table*> g_tables;
std::map<const void*, bool> g_ours;

constexpr ViewKind view_kind(std::size_t slot) {
    switch (slot) {
        case device_slot::kCreateShaderResourceView: return ViewKind::ShaderResource;
        case device_slot::kCreateRenderTargetView: return ViewKind::RenderTarget;
        case device_slot::kCreateDepthStencilView: return ViewKind::DepthStencil;
        default: return ViewKind::UnorderedAccess;
    }
}

void log_failed_buffer(const D3D11_BUFFER_DESC* d, HRESULT hr) {
    G6SHIM_WARN("CreateBuffer %u bytes, usage %d, bind 0x%X, cpu 0x%X, misc 0x%X, stride %u -> 0x%08lX", d->ByteWidth,
                static_cast<int>(d->Usage), d->BindFlags, d->CPUAccessFlags, d->MiscFlags, d->StructureByteStride,
                static_cast<unsigned long>(hr));
}

void log_failed_view(std::size_t slot, ID3D11Resource* resource, const void* desc, HRESULT hr) {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0, misc = 0;
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC t{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&t);
        format = t.Format;
        bind = t.BindFlags;
        misc = t.MiscFlags;
    }
    const auto* d = static_cast<const std::uint32_t*>(desc);
    G6SHIM_WARN("device slot %zu (Create*View): resource dimension %d format %d bind 0x%X misc 0x%X; view "
                "format %u dimension %u -> 0x%08lX",
                slot, static_cast<int>(dimension), static_cast<int>(format), bind, misc, d ? d[0] : 0u, d ? d[1] : 0u,
                static_cast<unsigned long>(hr));
}

}  // namespace

void use_console_layout(IUnknown* object, Layout layout) {
    if (!object) return;
    auto** slot = reinterpret_cast<const void* const**>(object);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ours.count(*slot)) return;
    Table*& table = g_tables[*slot];
    if (!table) {
        table = build(*slot, layout);
        g_ours[table->entries + 1] = true;
    }
    *slot = table->entries + 1;
}

template <std::size_t Slot, Layout L>
HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout(void* self, void* a, void* b, IUnknown** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*, IUnknown**);
    ViewDesc fixed{};
    if constexpr (L == Layout::View) b = const_cast<void*>(pc_view_desc(view_kind(Slot), b, fixed));
    const HRESULT hr = reinterpret_cast<Fn>(forward_thunk(Slot))(self, a, b, out);
    if (SUCCEEDED(hr) && out) use_console_layout(*out, L);
    if constexpr (Slot == device_slot::kCreateBuffer)
        if (SUCCEEDED(hr) && out) forget_placement(*out);
    if (FAILED(hr)) {
        if constexpr (Slot == device_slot::kCreateBuffer) {
            if (a) {
                log_failed_buffer(static_cast<const D3D11_BUFFER_DESC*>(a), hr);
                return hr;
            }
        } else if constexpr (L == Layout::View) {
            if (a) {
                log_failed_view(Slot, static_cast<ID3D11Resource*>(a), b, hr);
                return hr;
            }
        }
        G6SHIM_WARN("device slot %zu (creates a resource or view) -> 0x%08lX", Slot, static_cast<unsigned long>(hr));
    }
    return hr;
}

template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateBuffer, Layout::Resource>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateTexture1D, Layout::Resource>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateTexture3D, Layout::Resource>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateShaderResourceView, Layout::View>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateUnorderedAccessView, Layout::View>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateRenderTargetView, Layout::View>(void*, void*, void*, IUnknown**);
template HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout<device_slot::kCreateDepthStencilView, Layout::View>(void*, void*, void*, IUnknown**);

}  // namespace g6shim::d3d
