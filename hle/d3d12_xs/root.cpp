// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "root.hpp"

#include <g6shim/log.hpp>

#include <vector>

namespace g6shim::d3d12_xs::root {

namespace {

constexpr std::uint32_t kConsoleFlags = 0xFF00'0000;

using Serialize = HRESULT(WINAPI*)(const Desc*, std::uint32_t, void**, void**);
using SerializeVersioned = HRESULT(WINAPI*)(const VersionedDesc*, void**, void**);
using Deserialize = HRESULT(WINAPI*)(const std::uint8_t*, std::size_t, const Iid*, void**);

HMODULE host_module() {
    static const HMODULE module = [] {
        constexpr wchar_t kName[] = L"d3d12.dll";
        return ::LoadLibraryExW(kName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}

void* host_export(const char* name) {
    const HMODULE module = host_module();
    if (module == nullptr) return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(module, name));
}

Serialize host_serialize() {
    static const auto function = reinterpret_cast<Serialize>(host_export("D3D12SerializeRootSignature"));
    return function;
}

SerializeVersioned host_serialize_versioned() {
    static const auto function =
        reinterpret_cast<SerializeVersioned>(host_export("D3D12SerializeVersionedRootSignature"));
    return function;
}

Deserialize host_deserialize() {
    static const auto function =
        reinterpret_cast<Deserialize>(host_export("D3D12CreateRootSignatureDeserializer"));
    return function;
}

Desc without_console_flags(const Desc& desc) {
    if ((desc.flags & kConsoleFlags) != 0)
        G6SHIM_TRACE("d3d12_xs: console root signature flags 0x%X dropped", desc.flags & kConsoleFlags);
    Desc plain = desc;
    plain.flags &= ~kConsoleFlags;
    return plain;
}

const std::array<const void*, 24>& table();

struct RootSignature : Object {
    explicit RootSignature(std::vector<std::uint8_t> serialized)
        : Object(table().data()), blob(std::move(serialized)) {}

    std::vector<std::uint8_t> blob;
};

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kRootSignature, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_root(Object* object) { return release_boxed<RootSignature>(object); }

std::array<const void*, 24> make_functions() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kRootSignature, 24>());
    f[0] = reinterpret_cast<const void*>(&query);
    f[2] = reinterpret_cast<const void*>(&release_root);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    return f;
}

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> table = make_functions();
    return table;
}

}  // namespace

HRESULT serialize(const Desc* desc, std::uint32_t version, void** blob, void** error) {
    if (desc == nullptr) return E_INVALIDARG;
    const Serialize function = host_serialize();
    if (function == nullptr) {
        G6SHIM_WARN("d3d12_xs: no host d3d12.dll to serialize root signatures");
        return E_NOTIMPL;
    }
    const Desc plain = without_console_flags(*desc);
    return function(&plain, version, blob, error);
}

HRESULT serialize_versioned(const VersionedDesc* desc, void** blob, void** error) {
    if (desc == nullptr) return E_INVALIDARG;
    const SerializeVersioned function = host_serialize_versioned();
    if (function == nullptr) {
        G6SHIM_WARN("d3d12_xs: no host d3d12.dll to serialize a versioned root signature");
        return E_NOTIMPL;
    }
    VersionedDesc plain = *desc;
    plain.desc = without_console_flags(desc->desc);
    return function(&plain, blob, error);
}

HRESULT create_deserializer(const std::uint8_t* blob, std::size_t size, const Iid* iid, void** out) {
    if (blob == nullptr || out == nullptr) return E_INVALIDARG;
    const Deserialize function = host_deserialize();
    if (function == nullptr) {
        G6SHIM_WARN("d3d12_xs: no host d3d12.dll to deserialize a root signature");
        return E_NOTIMPL;
    }
    return function(blob, size, iid, out);
}

HRESULT create(Object*, std::uint32_t, const std::uint8_t* blob, std::size_t size, const Iid* iid, Object** out) {
    if (blob == nullptr || size == 0) return E_INVALIDARG;
    G6SHIM_TRACE("d3d12_xs: root signature of %zu bytes", size);
    std::vector<std::uint8_t> serialized(blob, blob + size);
    return hand_out(kRootSignature, std::make_unique<RootSignature>(std::move(serialized)), iid, kIids, out);
}

}  // namespace g6shim::d3d12_xs::root
