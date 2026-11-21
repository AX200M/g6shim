// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "console_objects.hpp"
#include "texture_shadow.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <initializer_list>
#include <new>
#include <string>
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
                               const std::vector<SlotOverride>& extra = {}) {
    std::vector<SlotOverride> overrides = {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&query_self<T>)},
                                           {kAddRefSlot, reinterpret_cast<const void*>(&add_ref_object<T>)},
                                           {kReleaseSlot, reinterpret_cast<const void*>(&release_child<T>)},
                                           {kGetDeviceSlot, reinterpret_cast<const void*>(&get_device<T>)}};
    overrides.insert(overrides.end(), extra.begin(), extra.end());
    return build_table({kind, offsetof(T, pc), pc_slots, slots}, overrides);
}

template <class T>
std::vector<SlotOverride> forwarded_slots(std::size_t slots, std::size_t (*pc_slot_of)(std::size_t)) {
    std::vector<SlotOverride> forwarded;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const std::size_t pc_slot = pc_slot_of(slot);
        if (pc_slot == kNoPcSlot) continue;
        if (const void* thunk = forward_thunk(offsetof(T, pc), pc_slot)) forwarded.emplace_back(slot, thunk);
    }
    return forwarded;
}

D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE heap_get_desc(D3D12_DESCRIPTOR_HEAP_DESC* result, ConsoleHeap* self) {
    *result = self->console_desc;
    return result;
}

SIZE_T STDMETHODCALLTYPE heap_cpu_start(ConsoleHeap* self) {
    return self->pc->GetCPUDescriptorHandleForHeapStart().ptr;
}

UINT64 STDMETHODCALLTYPE heap_gpu_start(ConsoleHeap* self) {
    if (!shader_visible(self->console_desc)) return kNoGpuDescriptorAddress;
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

bool is_texture(ID3D12Resource* resource) {
    return resource && resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
}

void record_barriers(ConsoleList* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers, bool copy_list) {
    if (count > kMaxBarriersPerCall || (count && !barriers)) {
        G6SHIM_WARN("ResourceBarrier(%u barriers): too many or null, ignored", count);
        return;
    }
    D3D12_RESOURCE_BARRIER kept[kMaxBarriersPerCall];
    UINT kept_count = 0;
    for (UINT i = 0; i < count; ++i) {
        const bool texture =
            barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && is_texture(barriers[i].Transition.pResource);
        D3D12_RESOURCE_BARRIER& pc = kept[kept_count];
        if (pc_transition(barriers[i], texture, &pc) && (!copy_list || kept_on_copy_list(pc))) ++kept_count;
    }
    if (kept_count) self->pc->ResourceBarrier(kept_count, kept);
}

UINT STDMETHODCALLTYPE list_resource_barrier(ConsoleList* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    record_barriers(self, count, barriers, false);
    return kNoBarrierActions;
}

void STDMETHODCALLTYPE list_execute_bundle(ConsoleList* self, void* bundle) {
    self->pc->ExecuteBundle(static_cast<ID3D12GraphicsCommandList*>(pc_list(bundle)));
}

void STDMETHODCALLTYPE list_set_descriptor_heaps(ConsoleList* self, UINT count, void* const* heaps) {
    if (count > kMaxBoundDescriptorHeaps || (count && !heaps)) {
        G6SHIM_WARN("SetDescriptorHeaps(%u heaps): too many or null, ignored", count);
        return;
    }
    ID3D12DescriptorHeap* pc[kMaxBoundDescriptorHeaps];
    for (UINT i = 0; i < count; ++i) pc[i] = pc_heap(heaps[i]);
    self->pc->SetDescriptorHeaps(count, pc);
}

UINT STDMETHODCALLTYPE dma_list_resource_barrier(ConsoleList* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    record_barriers(self, count, barriers, true);
    return kNoBarrierActions;
}

void STDMETHODCALLTYPE list_copy_texture_region_x(ConsoleList* self, const D3D12_TEXTURE_COPY_LOCATION* destination,
                                                  UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                                  const D3D12_BOX* box, UINT flags) {
    static std::atomic<int> logged{0};
    if (flags && logged.fetch_add(1, std::memory_order_relaxed) < kLoggedFlagWarnings)
        G6SHIM_WARN("CopyTextureRegionX: flags 0x%X ignored", flags);
    self->pc->CopyTextureRegion(destination, x, y, z, source, box);
}

void STDMETHODCALLTYPE list_flush_pipeline_x(ConsoleList*, UINT flags, UINT64 address, UINT64 bytes) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < kLoggedFlagWarnings)
        G6SHIM_INFO("FlushPipelineX(flags 0x%X, address 0x%llX, %llu bytes): ordered by barriers on the PC, nothing to do",
                    flags, address, bytes);
}

void log_recent_calls(const void* object) {
    const std::vector<std::uint32_t> calls = recent_calls(object);
    if (calls.empty()) return;
    std::string text;
    for (std::uint32_t slot : calls) text += " " + std::to_string(slot);
    G6SHIM_WARN("  recent console slots, oldest first:%s", text.c_str());
}

HRESULT STDMETHODCALLTYPE list_close(ConsoleList* self) {
    const HRESULT hr = self->pc->Close();
    static std::atomic<int> logged{0};
    if (FAILED(hr) && logged.fetch_add(1, std::memory_order_relaxed) < kLoggedFlagWarnings) {
        G6SHIM_WARN("Close(list type %u) -> 0x%08lX: the list recorded an invalid command", self->type, hr);
        log_recent_calls(self);
    }
    return hr;
}

D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE list_get_console_type(ConsoleList* self) {
    return static_cast<D3D12_COMMAND_LIST_TYPE>(self->type);
}

const void* const* list_table() {
    static const void* const* table = [] {
        std::vector<SlotOverride> slots = forwarded_slots<ConsoleList>(list_slot::kSlots, &pc_slot_of_list_slot);
        slots.insert(slots.end(),
                     {{list_slot::kGetType, reinterpret_cast<const void*>(&list_get_console_type)},
                      {list_slot::kResourceBarrier, reinterpret_cast<const void*>(&list_resource_barrier)},
                      {list_slot::kExecuteBundle, reinterpret_cast<const void*>(&list_execute_bundle)},
                      {list_slot::kSetDescriptorHeaps, reinterpret_cast<const void*>(&list_set_descriptor_heaps)},
                      {list_slot::kCopyTextureRegionX, reinterpret_cast<const void*>(&list_copy_texture_region_x)},
                      {list_slot::kFlushPipelineX, reinterpret_cast<const void*>(&list_flush_pipeline_x)},
                      {pc_list_slot::kClose, reinterpret_cast<const void*>(&list_close)}});
        return child_table<ConsoleList>("ID3D12GraphicsCommandList", list_slot::kMappedEnd, list_slot::kSlots, slots);
    }();
    return table;
}

const void* const* dma_list_table() {
    static const void* const* table = [] {
        std::vector<SlotOverride> slots =
            forwarded_slots<ConsoleList>(dma_list_slot::kSlots, &pc_slot_of_dma_list_slot);
        slots.insert(slots.end(),
                     {{dma_list_slot::kGetType, reinterpret_cast<const void*>(&list_get_console_type)},
                      {dma_list_slot::kResourceBarrier, reinterpret_cast<const void*>(&dma_list_resource_barrier)},
                      {pc_list_slot::kClose, reinterpret_cast<const void*>(&list_close)}});
        return child_table<ConsoleList>("DMA command list", dma_list_slot::kPcSlots, dma_list_slot::kSlots, slots);
    }();
    return table;
}

const void* const* heap_table() {
    static const void* const* table = child_table<ConsoleHeap>(
        "ID3D12DescriptorHeap", heap_slot::kPcSlots, heap_slot::kSlots,
        {{heap_slot::kGetDesc, reinterpret_cast<const void*>(&heap_get_desc)},
         {heap_slot::kGetCpuStart, reinterpret_cast<const void*>(&heap_cpu_start)},
         {heap_slot::kGetGpuStart, reinterpret_cast<const void*>(&heap_gpu_start)}});
    return table;
}

void STDMETHODCALLTYPE queue_execute_command_lists(ConsoleQueue* self, UINT count, void* const* lists) {
    if (count > kMaxExecutedLists || (count && !lists)) {
        G6SHIM_WARN("ExecuteCommandLists(%u lists): too many or null, ignored", count);
        return;
    }
    ID3D12CommandList* pc[kMaxExecutedLists];
    for (UINT i = 0; i < count; ++i) pc[i] = pc_list(lists[i]);
    flush_texture_shadows(self->device->pc, self->pc);
    self->pc->ExecuteCommandLists(count, pc);
}

}  // namespace

ID3D12CommandList* pc_list(void* list) {
    if (!list) return nullptr;
    const auto* head = static_cast<const GraphicsHead*>(list);
    if (head->table == list_table() || head->table == dma_list_table()) return static_cast<ConsoleList*>(list)->pc;
    return static_cast<ID3D12CommandList*>(list);
}

ID3D12DescriptorHeap* pc_heap(void* heap) {
    if (!heap) return nullptr;
    const auto* head = static_cast<const GraphicsHead*>(heap);
    if (head->table == heap_table()) return static_cast<ConsoleHeap*>(heap)->pc;
    return static_cast<ID3D12DescriptorHeap*>(heap);
}

ConsoleQueue* wrap_queue(ConsoleDevice* device, ID3D12CommandQueue* pc) {
    static const void* const* table = child_table<ConsoleQueue>(
        "ID3D12CommandQueue", queue_slot::kPcSlots, queue_slot::kSlots,
        {{queue_slot::kExecuteCommandLists, reinterpret_cast<const void*>(&queue_execute_command_lists)},
         {queue_slot::kPresentX, reinterpret_cast<const void*>(&queue_present_x)}});
    ConsoleQueue* queue = wrap_child<ConsoleQueue>(device, pc, table);
    if (queue) queue->type = static_cast<std::uint32_t>(pc->GetDesc().Type);
    return queue;
}

ConsoleList* wrap_list(ConsoleDevice* device, ID3D12GraphicsCommandList* pc) {
    ConsoleList* list = wrap_child<ConsoleList>(device, pc, list_table());
    if (list) list->type = static_cast<std::uint32_t>(pc->GetType());
    return list;
}

ConsoleList* wrap_dma_list(ConsoleDevice* device, ID3D12GraphicsCommandList* pc) {
    ConsoleList* list = wrap_child<ConsoleList>(device, pc, dma_list_table());
    if (list) list->type = kConsoleQueueTypeDma;
    return list;
}

ConsoleHeap* wrap_heap(ConsoleDevice* device, ID3D12DescriptorHeap* pc, const D3D12_DESCRIPTOR_HEAP_DESC& console_desc) {
    ConsoleHeap* heap = wrap_child<ConsoleHeap>(device, pc, heap_table());
    if (heap) heap->console_desc = console_desc;
    return heap;
}

}  // namespace g6shim::d3d12
