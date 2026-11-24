// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console_objects.hpp"

#include <g6shim/log.hpp>

#include <new>

namespace g6shim::d3d12 {

namespace {

bool is_adapter_iid(REFIID riid) {
    return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIAdapter) ||
           riid == __uuidof(IDXGIAdapter1);
}

HRESULT STDMETHODCALLTYPE adapter_query_interface(ConsoleAdapter* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!is_adapter_iid(riid)) return E_NOINTERFACE;
    add_ref(self->head);
    *out = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE adapter_add_ref(ConsoleAdapter* self) { return add_ref(self->head); }

ULONG STDMETHODCALLTYPE adapter_release(ConsoleAdapter* self) {
    const ULONG left = self->head.refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        self->pc->Release();
        delete self;
    }
    return left;
}

HRESULT hand_out_adapter(ConsoleDxgiDevice* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!is_adapter_iid(riid)) return E_NOINTERFACE;
    if (!self->adapter) {
        if (!self->device->adapter) return DXGI_ERROR_NOT_FOUND;
        self->adapter = wrap_adapter(self->device->adapter);
        if (!self->adapter) return E_OUTOFMEMORY;
    }
    add_ref(self->adapter->head);
    *out = self->adapter;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE dxgi_device_query_interface(ConsoleDxgiDevice* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1)) {
        add_ref(self->head);
        *out = self;
        return S_OK;
    }
    return self->device->head.table[kQueryInterfaceSlot]
               ? reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, REFIID, void**)>(
                     self->device->head.table[kQueryInterfaceSlot])(self->device, riid, out)
               : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE dxgi_device_add_ref(ConsoleDxgiDevice* self) { return add_ref(self->head); }

ULONG STDMETHODCALLTYPE dxgi_device_release(ConsoleDxgiDevice* self) {
    const ULONG left = self->head.refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        ConsoleDevice* device = self->device;
        if (device->dxgi == self) device->dxgi = nullptr;
        if (self->adapter) adapter_release(self->adapter);
        delete self;
        release_device(device);
    }
    return left;
}

HRESULT STDMETHODCALLTYPE dxgi_device_get_parent(ConsoleDxgiDevice* self, REFIID riid, void** out) {
    return hand_out_adapter(self, riid, out);
}

HRESULT STDMETHODCALLTYPE dxgi_device_get_adapter(ConsoleDxgiDevice* self, void** out) {
    return hand_out_adapter(self, __uuidof(IDXGIAdapter), out);
}

}  // namespace

ConsoleAdapter* wrap_adapter(IDXGIAdapter1* pc) {
    static const void* const* table =
        build_table({"IDXGIAdapter", offsetof(ConsoleAdapter, pc), dxgi_slot::kAdapterPcSlots, dxgi_slot::kSlots},
                    {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&adapter_query_interface)},
                     {kAddRefSlot, reinterpret_cast<const void*>(&adapter_add_ref)},
                     {kReleaseSlot, reinterpret_cast<const void*>(&adapter_release)}});
    if (!table) return nullptr;
    auto* adapter = new (std::nothrow) ConsoleAdapter{};
    if (!adapter) return nullptr;
    init_head(adapter->head, table);
    adapter->pc = pc;
    pc->AddRef();
    return adapter;
}

ConsoleDxgiDevice* wrap_dxgi_device(ConsoleDevice* device) {
    static const void* const* table =
        build_table({"IDXGIDevice", 0, 0, dxgi_slot::kSlots},
                    {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&dxgi_device_query_interface)},
                     {kAddRefSlot, reinterpret_cast<const void*>(&dxgi_device_add_ref)},
                     {kReleaseSlot, reinterpret_cast<const void*>(&dxgi_device_release)},
                     {dxgi_slot::kGetParent, reinterpret_cast<const void*>(&dxgi_device_get_parent)},
                     {dxgi_slot::kGetAdapter, reinterpret_cast<const void*>(&dxgi_device_get_adapter)}});
    if (!table) return nullptr;
    auto* dxgi = new (std::nothrow) ConsoleDxgiDevice{};
    if (!dxgi) return nullptr;
    init_head(dxgi->head, table);
    dxgi->device = device;
    add_ref(device->head);
    return dxgi;
}

}  // namespace g6shim::d3d12
