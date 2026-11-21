// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include "probe_object.hpp"

#include <windows.h>
#include <activation.h>
#include <inspectable.h>
#include <intrin.h>
#include <objidl.h>
#include <weakreference.h>
#include <winstring.h>

#include <g6shim/log.hpp>

#include <array>
#include <cstdio>
#include <utility>

namespace g6shim::winrt {

namespace {

constexpr GUID kIClosable = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};
constexpr std::size_t kCloseMethod = 0;
constexpr std::size_t kActivateInstanceMethod = 0;

bool refused(REFIID riid) { return riid == __uuidof(IMarshal) || riid == __uuidof(IWeakReferenceSource); }

bool is_identity(const Probe* self, REFIID riid) {
    return riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) || riid == __uuidof(IAgileObject) ||
           riid == self->iid;
}

HRESULT __stdcall QueryInterface(Probe* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (refused(riid)) return E_NOINTERFACE;
    if (is_identity(self, riid)) {
        ++self->refs;
        *out = self;
        return S_OK;
    }
    const bool stand_in = has_interface(self->class_name, riid);
    const std::string iid = guid_text(riid);
    log_once(self->class_name + iid + (stand_in ? "s" : "p"), "%s: QueryInterface %s -> %s", self->class_name.c_str(),
             iid.c_str(), stand_in ? "stand-in" : "probe");
    *out = make_probe(self->class_name, riid);
    return S_OK;
}

ULONG __stdcall AddRef(Probe* self) { return ++self->refs; }

ULONG __stdcall Release(Probe* self) {
    const ULONG n = --self->refs;
    if (n == 0) delete self;
    return n;
}

HRESULT __stdcall GetIids(Probe*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

HRESULT __stdcall GetRuntimeClassName(Probe* self, HSTRING* name) {
    if (!name) return E_POINTER;
    std::wstring w(self->class_name.begin(), self->class_name.end());
    return WindowsCreateString(w.c_str(), static_cast<UINT32>(w.size()), name);
}

HRESULT __stdcall GetTrustLevel(Probe*, TrustLevel* level) {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

std::string module_offset(const void* address) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    char buf[32];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        !GetModuleFileNameA(module, path, MAX_PATH)) {
        std::snprintf(buf, sizeof buf, "%p", address);
        return buf;
    }
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    std::snprintf(buf, sizeof buf, "+0x%llx",
                  static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    return std::string(name) + buf;
}

template <std::size_t N>
HRESULT __stdcall Method(Probe* self, void* a, void* b, void* c) {
    long result = 0;
    const EventCall call =
        auto_event(self, N, reinterpret_cast<std::uintptr_t>(a), reinterpret_cast<std::uintptr_t>(b), result);
    if (call != EventCall::None) {
        const std::string iid = guid_text(self->iid);
        log_once(self->class_name + iid + "#" + std::to_string(N),
                 "%s %s: method %zu (vtable slot %zu) taken as %s_<event>", self->class_name.c_str(), iid.c_str(), N,
                 N + kInspectableSlots, event_call_name(call));
        return result;
    }
    G6SHIM_WARN("probe %s %s: method %zu (vtable slot %zu) from %s, args %p %p %p -> E_NOTIMPL",
                self->class_name.c_str(), guid_text(self->iid).c_str(), N, N + kInspectableSlots,
                module_offset(_ReturnAddress()).c_str(), a, b, c);
    return E_NOTIMPL;
}

template <std::size_t... I>
void fill_methods(std::array<const void*, kMaxSlots>& t, std::index_sequence<I...>) {
    ((t[kInspectableSlots + I] = fn(&Method<I>)), ...);
}

HRESULT __stdcall ActivateInstance(Probe* self, IInspectable** instance) {
    if (!instance) return E_POINTER;
    *instance = static_cast<IInspectable*>(make_object(self->class_name, __uuidof(IInspectable), probe_vtable()));
    G6SHIM_WARN("probe %s: ActivateInstance -> probe instance", self->class_name.c_str());
    return S_OK;
}

HRESULT __stdcall Close(Probe*) { return S_OK; }

}  // namespace

const void* const* probe_vtable() {
    static const std::array<const void*, kMaxSlots> table = [] {
        std::array<const void*, kMaxSlots> t{};
        t[0] = fn(&QueryInterface);
        t[1] = fn(&AddRef);
        t[2] = fn(&Release);
        t[3] = fn(&GetIids);
        t[4] = fn(&GetRuntimeClassName);
        t[5] = fn(&GetTrustLevel);
        fill_methods(t, std::make_index_sequence<kMaxSlots - kInspectableSlots>{});
        return t;
    }();
    return table.data();
}

void* make_probe(const std::string& class_name, const GUID& iid) {
    if (const void* const* vtable = registered_vtable(class_name, iid)) return make_object(class_name, iid, vtable);
    if (iid == kIClosable) {
        static const void* const* closable = make_vtable({{kCloseMethod, fn(&Close)}});
        return make_object(class_name, iid, closable);
    }
    if (iid == __uuidof(IActivationFactory)) {
        static const void* const* factory = make_vtable({{kActivateInstanceMethod, fn(&ActivateInstance)}});
        return make_object(class_name, iid, factory);
    }
    return make_object(class_name, iid, probe_vtable());
}

}  // namespace g6shim::winrt
