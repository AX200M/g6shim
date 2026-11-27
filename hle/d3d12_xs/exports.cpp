// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.hpp"
#include "inplace.hpp"
#include "later.hpp"
#include "resource.hpp"
#include "root.hpp"

#include "../xmem/private.hpp"

#include <g6shim/log.hpp>
#include <g6shim/title_memory.hpp>

#include <windows.h>

#include <mutex>

namespace {

using namespace g6shim::d3d12_xs;

bool install_title_allocator() {
    HMODULE module = ::GetModuleHandleW(L"xmem.dll");
    if (module == nullptr) module = ::LoadLibraryW(L"xmem.dll");
    if (module == nullptr) {
        G6SHIM_WARN("d3d12_xs: xmem.dll is not loaded; resources use g6shim's own memory");
        return false;
    }
    const auto allocate = reinterpret_cast<resource::AllocRoutine>(::GetProcAddress(module, "XMemAlloc"));
    const auto free = reinterpret_cast<resource::FreeRoutine>(::GetProcAddress(module, "XMemFree"));
    if (allocate == nullptr || free == nullptr) {
        G6SHIM_WARN("d3d12_xs: xmem.dll has no XMemAlloc/XMemFree; resources use g6shim's own memory");
        return false;
    }
    resource::use_allocator(allocate, free);
    G6SHIM_INFO("d3d12_xs: resource memory comes from the title's allocator");
    return true;
}

g6shim::xmem::private_api::MapAlias g_map_alias = nullptr;

bool map_title_alias(std::uint64_t to, std::uint64_t from, std::uint64_t bytes) {
    if (g_map_alias == nullptr) return false;
    return g_map_alias(static_cast<std::uintptr_t>(to), static_cast<std::uintptr_t>(from),
                       static_cast<std::size_t>(bytes)) != 0;
}

bool install_page_mapper() {
    const g6shim::xmem::private_api::Exports exports = g6shim::xmem::private_api::resolve();
    if (!exports.map_alias) {
        G6SHIM_WARN("d3d12_xs: xmem.dll has no page mapper; page mappings stay as the title made them");
        return false;
    }
    g_map_alias = exports.map_alias;
    resource::use_page_mapper(&map_title_alias);
    if (exports.find_stretch) inplace::use_stretch_finder(exports.find_stretch);
    G6SHIM_INFO("d3d12_xs: page mappings come from xmem.dll");
    return true;
}

void prepare_device() {
    install_title_allocator();
    install_page_mapper();
    g6shim::set_title_memory_remap_observer(&later::title_memory_remapped);
}

}  // namespace

extern "C" HRESULT WINAPI g6shim_D3D12ConfigureVirtualMemoryX(g6shim::d3d12_xs::device::VirtualMemory* config) {
    return g6shim::d3d12_xs::device::configure_virtual_memory(config);
}

extern "C" HRESULT WINAPI g6shim_D3D12XboxCreateDevice(const void* adapter, const std::uint32_t* parameters,
                                                        const g6shim::d3d12_xs::Iid* iid,
                                                        g6shim::d3d12_xs::Object** out) {
    static std::once_flag prepared;
    std::call_once(prepared, prepare_device);
    return g6shim::d3d12_xs::device::create(adapter, parameters, iid, out);
}

extern "C" HRESULT WINAPI g6shim_D3D12SerializeRootSignature(const g6shim::d3d12_xs::root::Desc* desc,
                                                              std::uint32_t version, void** blob, void** error) {
    return g6shim::d3d12_xs::root::serialize(desc, version, blob, error);
}

extern "C" HRESULT WINAPI g6shim_D3D12SerializeVersionedRootSignature(
    const g6shim::d3d12_xs::root::VersionedDesc* desc, void** blob, void** error) {
    return g6shim::d3d12_xs::root::serialize_versioned(desc, blob, error);
}

extern "C" HRESULT WINAPI g6shim_D3D12CreateRootSignatureDeserializer(const std::uint8_t* blob, std::size_t size,
                                                                      const g6shim::d3d12_xs::Iid* iid,
                                                                      void** out) {
    return g6shim::d3d12_xs::root::create_deserializer(blob, size, iid, out);
}

extern "C" {
extern const g6shim::d3d12_xs::Iid g6shim_IID_ID3D12Device;
}
const g6shim::d3d12_xs::Iid g6shim_IID_ID3D12Device = g6shim::d3d12_xs::device::kIid;
