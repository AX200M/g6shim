// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "graphics_object.hpp"

#include <d3d12.h>

#include <g6shim/log.hpp>

#include <atomic>
#include <new>

namespace g6shim::d3d12 {

namespace {

inline constexpr std::size_t kBlobPcSlots = 5;
inline constexpr std::size_t kBlobSlots = 8;

struct ConsoleBlob {
    GraphicsHead head;
    void* buffer;
    std::uint64_t size;
    ID3DBlob* pc;
};
static_assert(offsetof(ConsoleBlob, buffer) == 0x10);
static_assert(offsetof(ConsoleBlob, size) == 0x18);

HRESULT STDMETHODCALLTYPE blob_query_interface(ConsoleBlob* self, REFIID, void** out) {
    if (!out) return E_POINTER;
    self->head.refs.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE blob_add_ref(ConsoleBlob* self) {
    return self->head.refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE blob_release(ConsoleBlob* self) {
    const ULONG left = self->head.refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        self->pc->Release();
        delete self;
    }
    return left;
}

ConsoleBlob* wrap_blob(ID3DBlob* pc) {
    static const void* const* table =
        build_table({"ID3DBlob", offsetof(ConsoleBlob, pc), kBlobPcSlots, kBlobSlots},
                    {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&blob_query_interface)},
                     {kAddRefSlot, reinterpret_cast<const void*>(&blob_add_ref)},
                     {kReleaseSlot, reinterpret_cast<const void*>(&blob_release)}});
    if (!table || !pc) return nullptr;
    auto* blob = new (std::nothrow) ConsoleBlob{};
    if (!blob) return nullptr;
    init_head(blob->head, table);
    blob->buffer = pc->GetBufferPointer();
    blob->size = pc->GetBufferSize();
    blob->pc = pc;
    return blob;
}

void hand_out(ID3DBlob* pc, void** out) {
    if (!out) {
        if (pc) pc->Release();
        return;
    }
    *out = pc ? wrap_blob(pc) : nullptr;
    if (pc && !*out) pc->Release();
}

}  // namespace

}  // namespace g6shim::d3d12

extern "C" HRESULT WINAPI g6shim_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                                             D3D_ROOT_SIGNATURE_VERSION version, void** blob,
                                                             void** error) {
    ID3DBlob* pc_blob = nullptr;
    ID3DBlob* pc_error = nullptr;
    const HRESULT hr = D3D12SerializeRootSignature(desc, version, &pc_blob, error ? &pc_error : nullptr);
    static std::atomic<int> logged{0};
    if (FAILED(hr))
        G6SHIM_WARN("D3D12SerializeRootSignature(%u parameters, flags 0x%X) -> 0x%08lX: %s",
                    desc ? desc->NumParameters : 0, desc ? desc->Flags : 0, hr,
                    pc_error ? static_cast<const char*>(pc_error->GetBufferPointer()) : "");
    else if (logged.fetch_add(1, std::memory_order_relaxed) == 0)
        G6SHIM_INFO("D3D12SerializeRootSignature: first root signature serialized");
    if (blob) *blob = nullptr;
    g6shim::d3d12::hand_out(pc_blob, blob);
    g6shim::d3d12::hand_out(pc_error, error);
    return hr;
}
