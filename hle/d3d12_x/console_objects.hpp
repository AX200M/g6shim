// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "graphics_object.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace g6shim::d3d12 {

namespace device_slot {
inline constexpr std::size_t kCreateCommandQueue = 8;
inline constexpr std::size_t kCreateCommandAllocator = 9;
inline constexpr std::size_t kCreateCommandList = 12;
inline constexpr std::size_t kCheckFeatureSupport = 13;
inline constexpr std::size_t kCreateDescriptorHeap = 14;
inline constexpr std::size_t kGetDescriptorHandleIncrementSize = 15;
inline constexpr std::size_t kCopyDescriptors = 23;
inline constexpr std::size_t kCopyDescriptorsSimple = 24;
inline constexpr std::size_t kPcSlots = 44;
inline constexpr std::size_t kCreateCommandQueueX = 48;
inline constexpr std::size_t kCreateCommandListX = 73;
inline constexpr std::size_t kSetFrameIntervalX = 78;
inline constexpr std::size_t kScheduleFrameEventX = 79;
inline constexpr std::size_t kSlots = 160;
}  // namespace device_slot

namespace queue_slot {
inline constexpr std::size_t kExecuteCommandLists = 10;
inline constexpr std::size_t kPcSlots = 18;
inline constexpr std::size_t kSlots = 64;
}  // namespace queue_slot

inline constexpr std::size_t kNoPcSlot = static_cast<std::size_t>(-1);

namespace pc_list_slot {
inline constexpr std::size_t kClose = 9;
inline constexpr std::size_t kReset = 10;
inline constexpr std::size_t kCopyBufferRegion = 15;
inline constexpr std::size_t kCopyTextureRegion = 16;
inline constexpr std::size_t kCopyResource = 17;
inline constexpr std::size_t kCopyTiles = 18;
inline constexpr std::size_t kResourceBarrier = 26;
inline constexpr std::size_t kSetDescriptorHeaps = 28;
inline constexpr std::size_t kBeginQuery = 52;
inline constexpr std::size_t kEndQuery = 53;
inline constexpr std::size_t kResolveQueryData = 54;
inline constexpr std::size_t kQueryAndPredicationSlots = 4;
inline constexpr std::size_t kSetMarker = 56;
inline constexpr std::size_t kBeginEvent = 57;
inline constexpr std::size_t kEndEvent = 58;
inline constexpr std::size_t kExecuteIndirect = 59;
}  // namespace pc_list_slot

namespace list_slot {
inline constexpr std::size_t kPcSlots = 9;
inline constexpr std::size_t kGetType = 8;
inline constexpr std::size_t kResourceBarrier = 26;
inline constexpr std::size_t kExecuteBundle = 27;
inline constexpr std::size_t kFirstQuery = 28;
inline constexpr std::size_t kSetDescriptorHeaps = kFirstQuery + pc_list_slot::kQueryAndPredicationSlots;
inline constexpr std::size_t kAfterMovedBlock = pc_list_slot::kBeginQuery + pc_list_slot::kQueryAndPredicationSlots;
inline constexpr std::size_t kMappedEnd = pc_list_slot::kExecuteIndirect + 1;
inline constexpr std::size_t kSlots = 160;
}  // namespace list_slot

constexpr std::size_t pc_slot_of_list_slot(std::size_t console_slot) {
    if (console_slot < list_slot::kFirstQuery) return console_slot;
    if (console_slot < list_slot::kSetDescriptorHeaps)
        return pc_list_slot::kBeginQuery + (console_slot - list_slot::kFirstQuery);
    if (console_slot < list_slot::kAfterMovedBlock) return console_slot - pc_list_slot::kQueryAndPredicationSlots;
    if (console_slot < list_slot::kMappedEnd) return console_slot;
    return kNoPcSlot;
}

namespace dma_list_slot {
inline constexpr std::size_t kPcSlots = 7;
inline constexpr std::size_t kGetType = 8;
inline constexpr std::size_t kFirstMapped = 9;
inline constexpr std::size_t kResourceBarrier = 15;
inline constexpr std::size_t kSlots = 48;
}  // namespace dma_list_slot

inline constexpr std::size_t kDmaListPcSlots[] = {
    pc_list_slot::kClose,          pc_list_slot::kReset,          pc_list_slot::kCopyBufferRegion,
    pc_list_slot::kCopyTextureRegion, pc_list_slot::kCopyResource, pc_list_slot::kCopyTiles,
    pc_list_slot::kResourceBarrier, pc_list_slot::kBeginQuery,    pc_list_slot::kEndQuery,
    pc_list_slot::kResolveQueryData, pc_list_slot::kSetMarker,    pc_list_slot::kBeginEvent,
    pc_list_slot::kEndEvent};

constexpr std::size_t pc_slot_of_dma_list_slot(std::size_t console_slot) {
    if (console_slot < dma_list_slot::kPcSlots) return console_slot;
    if (console_slot < dma_list_slot::kFirstMapped) return kNoPcSlot;
    const std::size_t index = console_slot - dma_list_slot::kFirstMapped;
    return index < std::size(kDmaListPcSlots) ? kDmaListPcSlots[index] : kNoPcSlot;
}

inline constexpr std::size_t kGetDeviceSlot = 7;

namespace heap_slot {
inline constexpr std::size_t kPcSlots = 7;
inline constexpr std::size_t kGetDesc = 8;
inline constexpr std::size_t kGetCpuStart = 9;
inline constexpr std::size_t kGetGpuStart = 10;
inline constexpr std::size_t kSlots = 16;
}  // namespace heap_slot

namespace dxgi_slot {
inline constexpr std::size_t kGetParent = 6;
inline constexpr std::size_t kGetAdapter = 7;
inline constexpr std::size_t kObjectPcSlots = 6;
inline constexpr std::size_t kAdapterPcSlots = 10;
inline constexpr std::size_t kSlots = 32;
}  // namespace dxgi_slot

inline constexpr std::size_t kDeviceConsoleBytes = 0x58;
inline constexpr std::size_t kQueueConsoleBytes = 0x18;
inline constexpr std::size_t kListConsoleBytes = 0x360;
inline constexpr std::size_t kHeapConsoleBytes = 0x10;
inline constexpr std::size_t kAdapterConsoleBytes = 0x18;
inline constexpr std::size_t kDxgiDeviceConsoleBytes = 0x20;
inline constexpr std::size_t kCreateDeviceParametersBytes = 0x28;
inline constexpr std::size_t kConsoleOptionsBytes = 0x40;
inline constexpr std::uint32_t kMaxExecutedLists = 256;
inline constexpr std::uint32_t kMaxBoundDescriptorHeaps = 2;
inline constexpr std::uint32_t kMaxBarriersPerCall = 256;
inline constexpr UINT kNoBarrierActions = 0;

inline constexpr std::uint32_t kConsoleQueueTypeDirect = 0;
inline constexpr std::uint32_t kConsoleQueueTypeCompute = 2;
inline constexpr std::uint32_t kConsoleQueueTypeDma = 16;

struct ConsoleQueueDesc {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint32_t engine_or_pipe;
    std::uint32_t queue_index;
};
static_assert(sizeof(ConsoleQueueDesc) == 16);

inline constexpr std::uint32_t kConsoleHeapTypeSamplerUnbounded = 4;

inline D3D12_DESCRIPTOR_HEAP_TYPE pc_heap_type(std::uint32_t console_type) {
    return console_type == kConsoleHeapTypeSamplerUnbounded ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                                            : static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(console_type);
}

inline D3D12_DESCRIPTOR_HEAP_DESC pc_heap_desc(const D3D12_DESCRIPTOR_HEAP_DESC& console) {
    D3D12_DESCRIPTOR_HEAP_DESC pc = console;
    pc.Type = pc_heap_type(static_cast<std::uint32_t>(console.Type));
    const bool visible_samplers =
        pc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && (pc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    if (visible_samplers && pc.NumDescriptors > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE)
        pc.NumDescriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    return pc;
}

inline constexpr D3D12_RESOURCE_STATES kCopyListStates = D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE;

inline bool copy_list_state(D3D12_RESOURCE_STATES state) { return (state & ~kCopyListStates) == 0; }

inline bool kept_on_copy_list(const D3D12_RESOURCE_BARRIER& barrier) {
    if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) return true;
    return copy_list_state(barrier.Transition.StateBefore) && copy_list_state(barrier.Transition.StateAfter);
}

struct ConsoleListDesc {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint32_t node_mask;
    std::uint32_t barrier_batch_size;
};
static_assert(sizeof(ConsoleListDesc) == 16);

struct ConsoleObjectList {
    std::uint32_t count;
    std::uint32_t reserved;
    HANDLE* objects;
};
static_assert(offsetof(ConsoleObjectList, objects) == 8);

inline bool pc_queue_type(std::uint32_t console_type, D3D12_COMMAND_LIST_TYPE* pc) {
    switch (console_type) {
    case kConsoleQueueTypeDirect: *pc = D3D12_COMMAND_LIST_TYPE_DIRECT; return true;
    case kConsoleQueueTypeCompute: *pc = D3D12_COMMAND_LIST_TYPE_COMPUTE; return true;
    case kConsoleQueueTypeDma: *pc = D3D12_COMMAND_LIST_TYPE_COPY; return true;
    default: return false;
    }
}

inline std::uint32_t pc_feature_data_bytes(D3D12_FEATURE feature, std::uint32_t console_bytes) {
    if (feature == D3D12_FEATURE_D3D12_OPTIONS && console_bytes == kConsoleOptionsBytes)
        return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS);
    return console_bytes;
}

struct ConsoleDxgiDevice;

struct ConsoleDevice {
    GraphicsHead head;
    std::uint64_t null_constant_buffer_address;
    unsigned char console_state[kDeviceConsoleBytes - 0x18];
    ID3D12Device* pc;
    IDXGIAdapter1* adapter;
    ConsoleDxgiDevice* dxgi;
};
static_assert(offsetof(ConsoleDevice, pc) == kDeviceConsoleBytes);

struct ConsoleQueue {
    GraphicsHead head;
    std::uint32_t type;
    std::uint32_t reserved;
    ID3D12CommandQueue* pc;
    ConsoleDevice* device;
};
static_assert(offsetof(ConsoleQueue, pc) == kQueueConsoleBytes);

struct ConsoleList {
    GraphicsHead head;
    std::uint32_t type;
    std::uint32_t reserved;
    unsigned char inline_state[kListConsoleBytes - 0x18];
    ID3D12GraphicsCommandList* pc;
    ConsoleDevice* device;
};
static_assert(offsetof(ConsoleList, pc) == kListConsoleBytes);

struct ConsoleHeap {
    GraphicsHead head;
    ID3D12DescriptorHeap* pc;
    ConsoleDevice* device;
    D3D12_DESCRIPTOR_HEAP_DESC console_desc;
};
static_assert(offsetof(ConsoleHeap, pc) == kHeapConsoleBytes);

struct ConsoleAdapter {
    GraphicsHead head;
    void* reserved;
    IDXGIAdapter1* pc;
};
static_assert(offsetof(ConsoleAdapter, pc) == kAdapterConsoleBytes);

struct ConsoleDxgiDevice {
    GraphicsHead head;
    void* reserved[2];
    ConsoleDevice* device;
    ConsoleAdapter* adapter;
};
static_assert(offsetof(ConsoleDxgiDevice, device) == kDxgiDeviceConsoleBytes);

HRESULT create_console_device(ID3D12Device* pc, IDXGIAdapter1* adapter, ConsoleDevice** out);
ConsoleQueue* wrap_queue(ConsoleDevice* device, ID3D12CommandQueue* pc);
ConsoleList* wrap_list(ConsoleDevice* device, ID3D12GraphicsCommandList* pc);
ConsoleList* wrap_dma_list(ConsoleDevice* device, ID3D12GraphicsCommandList* pc);
ID3D12CommandList* pc_list(void* list);
ID3D12DescriptorHeap* pc_heap(void* heap);
ConsoleHeap* wrap_heap(ConsoleDevice* device, ID3D12DescriptorHeap* pc, const D3D12_DESCRIPTOR_HEAP_DESC& console_desc);
ConsoleAdapter* wrap_adapter(IDXGIAdapter1* pc);
ConsoleDxgiDevice* wrap_dxgi_device(ConsoleDevice* device);

HRESULT STDMETHODCALLTYPE device_set_frame_interval(ConsoleDevice*, void* output, UINT interval_us, UINT periods,
                                                    UINT flags);
HRESULT STDMETHODCALLTYPE device_schedule_frame_event(ConsoleDevice*, UINT type, UINT offset_us,
                                                      const ConsoleObjectList* objects, UINT flags);

ULONG add_ref(GraphicsHead& head);
ULONG release_device(ConsoleDevice* device);

}  // namespace g6shim::d3d12
