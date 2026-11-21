// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "proxy.hpp"

#include <windows.h>
#include <intrin.h>

#include <g6shim/log.hpp>

#include <array>
#include <cstring>
#include <new>

namespace g6shim::d3d {

namespace {

constexpr std::size_t kThunkSize = 16;
constexpr std::size_t kFields = kProxyObjectSize / sizeof(void*);
constexpr std::uint8_t kBreakpoint = 0xCC;

constexpr std::uint8_t kLoadRealObject[] = {0x48, 0x8B, 0x49, 0x08};
constexpr std::uint8_t kLoadVtable[] = {0x48, 0x8B, 0x01};
constexpr std::uint8_t kJumpThroughVtable[] = {0xFF, 0xA0};

struct Proxy {
    const void* const* vtable;
    IUnknown* real;
    const char* name;
    std::size_t base;
};
static_assert(sizeof(Proxy) <= kProxyFirstField);
static_assert(offsetof(Proxy, real) == kLoadRealObject[3]);

template <std::size_t N>
HRESULT STDMETHODCALLTYPE Extension(Proxy* self, void* a, void* b, void* c) {
    G6SHIM_WARN("%s: console-only method, vtable slot %zu, called from %p, args %p %p %p -> E_NOTIMPL", self->name, N,
                _ReturnAddress(), a, b, c);
    return E_NOTIMPL;
}

template <std::size_t... I>
std::array<const void*, kProxySlots> extension_table(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&Extension<I>)...};
}

template <std::size_t N>
HRESULT STDMETHODCALLTYPE Field(Proxy* self, void* a, void* b, void* c) {
    G6SHIM_WARN("%s: console-only function at object offset +0x%zX, called from %p, args %p %p %p", self->name,
                N * sizeof(void*), _ReturnAddress(), a, b, c);
    return E_NOTIMPL;
}

template <std::size_t... I>
std::array<const void*, kFields> field_table(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&Field<I>)...};
}

void write_thunk(std::uint8_t* t, std::size_t slot) {
    const auto disp = static_cast<std::uint32_t>(slot * sizeof(void*));
    std::uint8_t* p = t;
    std::memcpy(p, kLoadRealObject, sizeof kLoadRealObject);
    p += sizeof kLoadRealObject;
    std::memcpy(p, kLoadVtable, sizeof kLoadVtable);
    p += sizeof kLoadVtable;
    std::memcpy(p, kJumpThroughVtable, sizeof kJumpThroughVtable);
    p += sizeof kJumpThroughVtable;
    std::memcpy(p, &disp, sizeof disp);
    p += sizeof disp;
    std::memset(p, kBreakpoint, kThunkSize - static_cast<std::size_t>(p - t));
}

const std::uint8_t* thunks() {
    static const std::uint8_t* code = [] {
        auto* mem = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, kProxySlots * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!mem) return static_cast<const std::uint8_t*>(nullptr);
        for (std::size_t s = 0; s < kProxySlots; ++s) write_thunk(mem + s * kThunkSize, s);
        DWORD old = 0;
        VirtualProtect(mem, kProxySlots * kThunkSize, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), mem, kProxySlots * kThunkSize);
        return static_cast<const std::uint8_t*>(mem);
    }();
    return code;
}

}  // namespace

IUnknown* real_object(void* proxy) { return static_cast<Proxy*>(proxy)->real; }

const void* forward_thunk(std::size_t slot) {
    const std::uint8_t* code = thunks();
    return code && slot < kProxySlots ? code + slot * kThunkSize : nullptr;
}

void* make_proxy(IUnknown* real, std::size_t base, const char* name, std::initializer_list<Override> overrides,
                 const std::vector<Override>& fields) {
    const std::uint8_t* code = thunks();
    if (!code || base > kProxySlots) return nullptr;
    static const auto extensions = extension_table(std::make_index_sequence<kProxySlots>{});
    auto* vtable = new std::array<const void*, kProxySlots>();
    for (std::size_t s = 0; s < kProxySlots; ++s)
        (*vtable)[s] = s < base ? static_cast<const void*>(code + s * kThunkSize) : extensions[s];
    for (const auto& [slot, fn] : overrides)
        if (slot < kProxySlots) (*vtable)[slot] = fn;
    real->AddRef();

    static const auto field_probes = field_table(std::make_index_sequence<kFields>{});
    auto* block = static_cast<const void**>(VirtualAlloc(nullptr, kProxyObjectSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) return nullptr;
    for (std::size_t i = 0; i < kFields; ++i) block[i] = field_probes[i];
    for (const auto& [offset, fn] : fields)
        if (offset % sizeof(void*) == 0 && offset < kProxyObjectSize) block[offset / sizeof(void*)] = fn;
    return new (block) Proxy{vtable->data(), real, name, base};
}

}  // namespace g6shim::d3d
