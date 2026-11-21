// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "console_objects.hpp"
#include "constant_views.hpp"
#include "removal.hpp"
#include "texture_shadow.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace g6shim::d3d12 {

namespace {

constexpr int kLoggedCreations = 8;

void log_iid(const char* where, REFIID riid) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCreations)
        G6SHIM_WARN("%s: interface {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} not known", where, riid.Data1,
                    riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4],
                    riid.Data4[5], riid.Data4[6], riid.Data4[7]);
}

bool is_device_iid(REFIID riid) {
    return riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device);
}

bool is_dxgi_device_iid(REFIID riid) {
    return riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1);
}

HRESULT STDMETHODCALLTYPE device_query_interface(ConsoleDevice* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (is_device_iid(riid)) {
        add_ref(self->head);
        *out = self;
        return S_OK;
    }
    if (is_dxgi_device_iid(riid)) {
        if (self->dxgi) {
            add_ref(self->dxgi->head);
        } else {
            self->dxgi = wrap_dxgi_device(self);
            if (!self->dxgi) return E_OUTOFMEMORY;
        }
        *out = self->dxgi;
        return S_OK;
    }
    log_iid("ID3D12Device::QueryInterface", riid);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE device_add_ref(ConsoleDevice* self) { return add_ref(self->head); }

ULONG STDMETHODCALLTYPE device_release(ConsoleDevice* self) { return release_device(self); }

HRESULT STDMETHODCALLTYPE device_create_command_queue(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                      REFIID riid, void** out) {
    if (out) *out = nullptr;
    ID3D12CommandQueue* pc = nullptr;
    HRESULT hr = self->pc->CreateCommandQueue(desc, IID_PPV_ARGS(&pc));
    G6SHIM_INFO("CreateCommandQueue(type %d, priority %d, flags 0x%X) -> 0x%08lX", desc ? desc->Type : -1,
                desc ? desc->Priority : 0, desc ? desc->Flags : 0, hr);
    if (FAILED(hr) || !out) {
        if (pc) pc->Release();
        return FAILED(hr) ? hr : S_FALSE;
    }
    if (riid != __uuidof(ID3D12CommandQueue)) log_iid("CreateCommandQueue", riid);
    ConsoleQueue* queue = wrap_queue(self, pc);
    pc->Release();
    if (!queue) return E_OUTOFMEMORY;
    *out = queue;
    return S_OK;
}

HRESULT create_list(ConsoleDevice* self, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
                    ID3D12PipelineState* state, REFIID riid, void** out, bool dma) {
    if (out) *out = nullptr;
    ID3D12GraphicsCommandList* pc = nullptr;
    HRESULT hr = self->pc->CreateCommandList(node_mask, type, allocator, state, IID_PPV_ARGS(&pc));
    static std::atomic<int> logged{0};
    if (FAILED(hr) || logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCreations)
        G6SHIM_INFO("CreateCommandList(type %d%s) -> 0x%08lX", type, dma ? ", console DMA list" : "", hr);
    if (FAILED(hr) || !out) {
        if (pc) pc->Release();
        return FAILED(hr) ? hr : S_FALSE;
    }
    if (riid != __uuidof(ID3D12GraphicsCommandList) && riid != __uuidof(ID3D12CommandList))
        log_iid("CreateCommandList", riid);
    ConsoleList* list = dma ? wrap_dma_list(self, pc) : wrap_list(self, pc);
    pc->Release();
    if (!list) return E_OUTOFMEMORY;
    *out = list;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE device_create_command_list(ConsoleDevice* self, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                                     ID3D12CommandAllocator* allocator, ID3D12PipelineState* state,
                                                     REFIID riid, void** out) {
    D3D12_COMMAND_LIST_TYPE pc_type{};
    const bool console_type = pc_queue_type(static_cast<std::uint32_t>(type), &pc_type);
    return create_list(self, node_mask, console_type ? pc_type : type, allocator, state, riid, out,
                       static_cast<std::uint32_t>(type) == kConsoleQueueTypeDma);
}

HRESULT STDMETHODCALLTYPE device_create_command_allocator(ConsoleDevice* self, UINT type, REFIID riid, void** out) {
    D3D12_COMMAND_LIST_TYPE pc_type{};
    if (!pc_queue_type(type, &pc_type)) pc_type = static_cast<D3D12_COMMAND_LIST_TYPE>(type);
    const HRESULT hr = self->pc->CreateCommandAllocator(pc_type, riid, out);
    if (FAILED(hr)) {
        G6SHIM_WARN("CreateCommandAllocator(type %u) -> 0x%08lX", type, hr);
        report_device_removal(self->pc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE device_create_descriptor_heap(ConsoleDevice* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                        REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!desc) return E_INVALIDARG;
    const D3D12_DESCRIPTOR_HEAP_DESC pc_desc = pc_heap_desc(*desc);
    ID3D12DescriptorHeap* pc = nullptr;
    HRESULT hr = self->pc->CreateDescriptorHeap(&pc_desc, IID_PPV_ARGS(&pc));
    static std::atomic<int> logged{0};
    if (FAILED(hr) || pc_desc.NumDescriptors != desc->NumDescriptors ||
        logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCreations)
        G6SHIM_INFO("CreateDescriptorHeap(type %d, %u descriptors, flags 0x%X) as PC type %d with %u -> 0x%08lX",
                    desc->Type, desc->NumDescriptors, desc->Flags, pc_desc.Type, pc_desc.NumDescriptors, hr);
    if (FAILED(hr) || !out) {
        if (pc) pc->Release();
        return FAILED(hr) ? hr : S_FALSE;
    }
    if (riid != __uuidof(ID3D12DescriptorHeap)) log_iid("CreateDescriptorHeap", riid);
    ConsoleHeap* heap = wrap_heap(self, pc, *desc);
    pc->Release();
    if (!heap) return E_OUTOFMEMORY;
    *out = heap;
    return S_OK;
}

UINT STDMETHODCALLTYPE device_descriptor_increment(ConsoleDevice* self, UINT type) {
    return self->pc->GetDescriptorHandleIncrementSize(pc_heap_type(type));
}

void STDMETHODCALLTYPE device_copy_descriptors(ConsoleDevice* self, UINT dest_ranges,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts, const UINT* dest_sizes,
                                               UINT src_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                               const UINT* src_sizes, UINT type) {
    self->pc->CopyDescriptors(dest_ranges, dest_starts, dest_sizes, src_ranges, src_starts, src_sizes,
                              pc_heap_type(type));
}

void STDMETHODCALLTYPE device_copy_descriptors_simple(ConsoleDevice* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE src, UINT type) {
    self->pc->CopyDescriptorsSimple(count, dest, src, pc_heap_type(type));
}

HRESULT STDMETHODCALLTYPE device_create_command_queue_x(ConsoleDevice* self, const ConsoleQueueDesc* desc, REFIID riid,
                                                        void** out) {
    if (out) *out = nullptr;
    if (!desc) return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC pc_desc{};
    if (!pc_queue_type(desc->type, &pc_desc.Type)) {
        G6SHIM_WARN("CreateCommandQueueX: console queue type %u not known -> E_INVALIDARG", desc->type);
        return E_INVALIDARG;
    }
    G6SHIM_INFO("CreateCommandQueueX(type %u, flags 0x%X, engine or pipe %u, queue %u) as PC type %d", desc->type,
                desc->flags, desc->engine_or_pipe, desc->queue_index, pc_desc.Type);
    return device_create_command_queue(self, &pc_desc, riid, out);
}

HRESULT STDMETHODCALLTYPE device_create_command_list_x(ConsoleDevice* self, const ConsoleListDesc* desc,
                                                       ID3D12CommandAllocator* allocator, ID3D12PipelineState* state,
                                                       REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!desc) return E_INVALIDARG;
    D3D12_COMMAND_LIST_TYPE type{};
    if (!pc_queue_type(desc->type, &type)) {
        G6SHIM_WARN("CreateCommandListX: console list type %u not known -> E_INVALIDARG", desc->type);
        return E_INVALIDARG;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCreations)
        G6SHIM_INFO("CreateCommandListX(type %u, flags 0x%X, node mask 0x%X, barrier batch %u)", desc->type, desc->flags,
                    desc->node_mask, desc->barrier_batch_size);
    return create_list(self, 0, type, allocator, state, riid, out, desc->type == kConsoleQueueTypeDma);
}

HRESULT STDMETHODCALLTYPE device_check_feature_support(ConsoleDevice* self, D3D12_FEATURE feature, void* data,
                                                       UINT bytes) {
    const UINT pc_bytes = pc_feature_data_bytes(feature, bytes);
    if (pc_bytes != bytes && data) std::memset(static_cast<unsigned char*>(data) + pc_bytes, 0, bytes - pc_bytes);
    const HRESULT hr = self->pc->CheckFeatureSupport(feature, data, pc_bytes);
    if (FAILED(hr)) G6SHIM_WARN("CheckFeatureSupport(feature %d, %u bytes) -> 0x%08lX", feature, bytes, hr);
    return hr;
}

HRESULT tracked(HRESULT hr, const D3D12_RESOURCE_DESC* desc, void** out) {
    if (SUCCEEDED(hr) && out && *out && desc && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        track_texture(static_cast<ID3D12Resource*>(*out));
    return hr;
}

void STDMETHODCALLTYPE device_create_constant_buffer_view(ConsoleDevice* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
    if (!desc || constant_view_fits_pc(desc->BufferLocation, desc->SizeInBytes)) {
        self->pc->CreateConstantBufferView(desc, descriptor);
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC pc = *desc;
    pc.SizeInBytes = pc_constant_view_size(desc->SizeInBytes);
    if (!relocate_constants(self->pc, desc->BufferLocation, desc->SizeInBytes, &pc.BufferLocation)) {
        pc.BufferLocation = desc->BufferLocation & ~UINT64{kConstantViewAlignment - 1};
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < kLoggedCreations)
            G6SHIM_WARN("CreateConstantBufferView(0x%llX, %u bytes): not in an upload buffer, view moved to 0x%llX",
                        desc->BufferLocation, desc->SizeInBytes, pc.BufferLocation);
    }
    self->pc->CreateConstantBufferView(&pc, descriptor);
}

const D3D12_RESOURCE_DESC* pc_resource_desc_or_null(const D3D12_RESOURCE_DESC* console, D3D12_RESOURCE_DESC* storage) {
    if (!console) return nullptr;
    *storage = pc_resource_desc(*console);
    return storage;
}

HRESULT STDMETHODCALLTYPE device_create_committed_resource(ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap,
                                                           D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* desc,
                                                           D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                           REFIID riid, void** out) {
    D3D12_RESOURCE_DESC pc_desc{};
    const HRESULT hr =
        tracked(self->pc->CreateCommittedResource(heap, heap_flags, pc_resource_desc_or_null(desc, &pc_desc),
                                                  pc_initial_state(desc, state), clear, riid, out),
                desc, out);
    if (SUCCEEDED(hr) && out && *out && heap && heap->Type == D3D12_HEAP_TYPE_UPLOAD && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        track_upload_buffer(static_cast<ID3D12Resource*>(*out), desc->Width);
    return hr;
}

HRESULT STDMETHODCALLTYPE device_create_placed_resource(ConsoleDevice* self, ID3D12Heap* heap, UINT64 offset,
                                                        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                        const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    D3D12_RESOURCE_DESC pc_desc{};
    return tracked(self->pc->CreatePlacedResource(heap, offset, pc_resource_desc_or_null(desc, &pc_desc),
                                                  pc_initial_state(desc, state), clear, riid, out),
                   desc, out);
}

HRESULT STDMETHODCALLTYPE device_create_reserved_resource(ConsoleDevice* self, const D3D12_RESOURCE_DESC* desc,
                                                          D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                          REFIID riid, void** out) {
    D3D12_RESOURCE_DESC pc_desc{};
    return tracked(self->pc->CreateReservedResource(pc_resource_desc_or_null(desc, &pc_desc),
                                                    pc_initial_state(desc, state), clear, riid, out),
                   desc, out);
}

D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE device_resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO* result,
                                                                                ConsoleDevice* self, UINT visible_mask,
                                                                                UINT count,
                                                                                const D3D12_RESOURCE_DESC* descs) {
    if (count > kMaxAllocationInfoDescs || (count && !descs)) {
        G6SHIM_WARN("GetResourceAllocationInfo(%u descriptions): too many or null", count);
        *result = {kInvalidAllocationSize, 0};
        return result;
    }
    D3D12_RESOURCE_DESC pc[kMaxAllocationInfoDescs];
    for (UINT i = 0; i < count; ++i) pc[i] = pc_resource_desc(descs[i]);
    *result = self->pc->GetResourceAllocationInfo(visible_mask, count, pc);
    return result;
}

D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE device_custom_heap_properties(D3D12_HEAP_PROPERTIES* result,
                                                                       ConsoleDevice* self, UINT node_mask,
                                                                       D3D12_HEAP_TYPE type) {
    *result = self->pc->GetCustomHeapProperties(node_mask, type);
    return result;
}

LUID STDMETHODCALLTYPE device_adapter_luid(ConsoleDevice* self) { return self->pc->GetAdapterLuid(); }

const void* const* device_table() {
    static const void* const* table = build_table(
        {"ID3D12Device", offsetof(ConsoleDevice, pc), device_slot::kPcSlots, device_slot::kSlots},
        {{kQueryInterfaceSlot, reinterpret_cast<const void*>(&device_query_interface)},
         {kAddRefSlot, reinterpret_cast<const void*>(&device_add_ref)},
         {kReleaseSlot, reinterpret_cast<const void*>(&device_release)},
         {device_slot::kCreateCommandQueue, reinterpret_cast<const void*>(&device_create_command_queue)},
         {device_slot::kCreateCommandAllocator, reinterpret_cast<const void*>(&device_create_command_allocator)},
         {device_slot::kCreateCommandList, reinterpret_cast<const void*>(&device_create_command_list)},
         {device_slot::kCheckFeatureSupport, reinterpret_cast<const void*>(&device_check_feature_support)},
         {device_slot::kCreateDescriptorHeap, reinterpret_cast<const void*>(&device_create_descriptor_heap)},
         {device_slot::kGetDescriptorHandleIncrementSize, reinterpret_cast<const void*>(&device_descriptor_increment)},
         {device_slot::kCopyDescriptors, reinterpret_cast<const void*>(&device_copy_descriptors)},
         {device_slot::kCopyDescriptorsSimple, reinterpret_cast<const void*>(&device_copy_descriptors_simple)},
         {device_slot::kGetResourceAllocationInfo, reinterpret_cast<const void*>(&device_resource_allocation_info)},
         {device_slot::kGetCustomHeapProperties, reinterpret_cast<const void*>(&device_custom_heap_properties)},
         {device_slot::kGetAdapterLuid, reinterpret_cast<const void*>(&device_adapter_luid)},
         {device_slot::kCreateConstantBufferView, reinterpret_cast<const void*>(&device_create_constant_buffer_view)},
         {device_slot::kCreateCommittedResource, reinterpret_cast<const void*>(&device_create_committed_resource)},
         {device_slot::kCreatePlacedResource, reinterpret_cast<const void*>(&device_create_placed_resource)},
         {device_slot::kCreateReservedResource, reinterpret_cast<const void*>(&device_create_reserved_resource)},
         {device_slot::kCreateCommandQueueX, reinterpret_cast<const void*>(&device_create_command_queue_x)},
         {device_slot::kCreateCommandListX, reinterpret_cast<const void*>(&device_create_command_list_x)},
         {device_slot::kSetFrameIntervalX, reinterpret_cast<const void*>(&device_set_frame_interval)},
         {device_slot::kScheduleFrameEventX, reinterpret_cast<const void*>(&device_schedule_frame_event)},
         {device_slot::kWaitFrameEventX, reinterpret_cast<const void*>(&device_wait_frame_event)}});
    return table;
}

}  // namespace

ULONG add_ref(GraphicsHead& head) { return head.refs.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG release_device(ConsoleDevice* device) {
    const ULONG left = device->head.refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        device->pc->Release();
        if (device->adapter) device->adapter->Release();
        delete device;
    }
    return left;
}

HRESULT create_console_device(ID3D12Device* pc, IDXGIAdapter1* adapter, ConsoleDevice** out) {
    const void* const* table = device_table();
    if (!table) return E_OUTOFMEMORY;
    auto* device = new (std::nothrow) ConsoleDevice{};
    if (!device) return E_OUTOFMEMORY;
    init_head(device->head, table);
    device->pc = pc;
    pc->AddRef();
    device->adapter = adapter;
    if (adapter) adapter->AddRef();
    *out = device;
    return S_OK;
}

}  // namespace g6shim::d3d12

namespace {

using namespace g6shim::d3d12;

std::atomic<UINT> g_process_debug_flags{0};

bool debug_layer_requested() {
    const char* value = std::getenv("G6SHIM_D3D_DEBUG");
    return value && *value && *value != '0';
}

void log_parameters(const void* params) {
    std::uint32_t words[kCreateDeviceParametersBytes / 4];
    std::memcpy(words, params, sizeof words);
    G6SHIM_INFO("D3D12XboxCreateDevice parameters: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", words[0],
                words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9]);
}

HRESULT open_hardware_adapter(IDXGIAdapter1** out) {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    hr = DXGI_ERROR_NOT_FOUND;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            G6SHIM_INFO("D3D12XboxCreateDevice: adapter %ls", desc.Description);
            *out = adapter;
            hr = S_OK;
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return hr;
}

}  // namespace

extern "C" HRESULT WINAPI g6shim_D3D12XboxSetProcessDebugFlags(UINT flags) {
    g_process_debug_flags.store(flags, std::memory_order_relaxed);
    G6SHIM_INFO("D3D12XboxSetProcessDebugFlags(0x%X)", flags);
    return S_OK;
}

extern "C" HRESULT WINAPI g6shim_D3D12XboxCreateDevice(void* console_adapter, const void* params, REFIID riid,
                                                       void** out) {
    if (!params) return E_INVALIDARG;
    if (out) *out = nullptr;
    log_parameters(params);
    if (console_adapter) G6SHIM_WARN("D3D12XboxCreateDevice: adapter %p ignored", console_adapter);
    enable_removal_diagnostics();
    if (debug_layer_requested()) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
    IDXGIAdapter1* adapter = nullptr;
    HRESULT hr = open_hardware_adapter(&adapter);
    if (FAILED(hr)) {
        G6SHIM_ERROR("D3D12XboxCreateDevice: no Direct3D 12 adapter -> 0x%08lX", hr);
        return hr;
    }
    ID3D12Device* pc = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pc));
    if (FAILED(hr) || !out) {
        G6SHIM_INFO("D3D12XboxCreateDevice -> 0x%08lX", hr);
        if (pc) pc->Release();
        adapter->Release();
        return FAILED(hr) ? hr : S_FALSE;
    }
    if (!is_device_iid(riid)) log_iid("D3D12XboxCreateDevice", riid);
    ConsoleDevice* device = nullptr;
    hr = create_console_device(pc, adapter, &device);
    pc->Release();
    adapter->Release();
    if (SUCCEEDED(hr)) *out = device;
    G6SHIM_INFO("D3D12XboxCreateDevice -> 0x%08lX, %p", hr, out ? *out : nullptr);
    return hr;
}
