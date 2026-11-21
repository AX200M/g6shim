// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "console_objects.hpp"

#include <initializer_list>
#include <new>
#include <vector>

namespace g6shim::d3d12 {

namespace {

template <class T>
HRESULT STDMETHODCALLTYPE query_self(T* self, REFIID, void** out) {
    if (!out) return E_POINTER;
    add_ref(self->head);
    *out = self;
    return S_OK;
}

template <class T>
ULONG STDMETHODCALLTYPE add_ref_object(T* self) {
    return add_ref(self->head);
}

template <class T>
ULONG STDMETHODCALLTYPE release_child(T* self) {
    const ULONG left = self->head.refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        self->pc->Release();
        ConsoleDevice* device = self->device;
        delete self;
        release_device(device);
    }
    return left;
}

template <class T>
HRESULT STDMETHODCALLTYPE get_device(T* self, REFIID riid, void** out) {
    using QueryInterface = HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, REFIID, void**);
    return reinterpret_cast<QueryInterface>(self->device->head.table[kQueryInterfaceSlot])(self->device, riid, out);
}

template <class T>
const void* const* child_table(const char* kind, std::size_t pc_slots, std::size_t slots,
                               std::initializer_list<SlotOverride> extra = {}) {
    std::vector<SlotOverride> overrides = {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&query_self<T>)},
                                           {kAddRefSlot, reinterpret_cast<const void*>(&add_ref_object<T>)},
                                           {kReleaseSlot, reinterpret_cast<const void*>(&release_child<T>)},
                                           {kGetDeviceSlot, reinterpret_cast<const void*>(&get_device<T>)}};
    overrides.insert(overrides.end(), extra.begin(), extra.end());
    return build_table({kind, offsetof(T, pc), pc_slots, slots}, overrides);
}

D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE heap_get_desc(D3D12_DESCRIPTOR_HEAP_DESC* result, ConsoleHeap* self) {
    *result = self->pc->GetDesc();
    return result;
}

SIZE_T STDMETHODCALLTYPE heap_cpu_start(ConsoleHeap* self) {
    return self->pc->GetCPUDescriptorHandleForHeapStart().ptr;
}

UINT64 STDMETHODCALLTYPE heap_gpu_start(ConsoleHeap* self) {
    return self->pc->GetGPUDescriptorHandleForHeapStart().ptr;
}

template <class T, class Pc>
T* wrap_child(ConsoleDevice* device, Pc* pc, const void* const* table) {
    if (!table) return nullptr;
    auto* object = new (std::nothrow) T{};
    if (!object) return nullptr;
    init_head(object->head, table);
    object->pc = pc;
    pc->AddRef();
    object->device = device;
    add_ref(device->head);
    return object;
}

}  // namespace

ConsoleQueue* wrap_queue(ConsoleDevice* device, ID3D12CommandQueue* pc) {
    static const void* const* table =
        child_table<ConsoleQueue>("ID3D12CommandQueue", queue_slot::kPcSlots, queue_slot::kSlots);
    ConsoleQueue* queue = wrap_child<ConsoleQueue>(device, pc, table);
    if (queue) queue->type = static_cast<std::uint32_t>(pc->GetDesc().Type);
    return queue;
}

ConsoleList* wrap_list(ConsoleDevice* device, ID3D12GraphicsCommandList* pc) {
    static const void* const* table =
        child_table<ConsoleList>("ID3D12GraphicsCommandList", list_slot::kPcSlots, list_slot::kSlots);
    ConsoleList* list = wrap_child<ConsoleList>(device, pc, table);
    if (list) list->type = static_cast<std::uint32_t>(pc->GetType());
    return list;
}

ConsoleHeap* wrap_heap(ConsoleDevice* device, ID3D12DescriptorHeap* pc) {
    static const void* const* table = child_table<ConsoleHeap>(
        "ID3D12DescriptorHeap", heap_slot::kPcSlots, heap_slot::kSlots,
        {{heap_slot::kGetDesc, reinterpret_cast<const void*>(&heap_get_desc)},
         {heap_slot::kGetCpuStart, reinterpret_cast<const void*>(&heap_cpu_start)},
         {heap_slot::kGetGpuStart, reinterpret_cast<const void*>(&heap_gpu_start)}});
    return wrap_child<ConsoleHeap>(device, pc, table);
}

}  // namespace g6shim::d3d12
