// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "graphics_object.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::d3d12 {

namespace device_slot {
inline constexpr std::size_t kCreateCommandQueue = 8;
inline constexpr std::size_t kCreateCommandList = 12;
inline constexpr std::size_t kCheckFeatureSupport = 13;
inline constexpr std::size_t kCreateDescriptorHeap = 14;
inline constexpr std::size_t kPcSlots = 44;
inline constexpr std::size_t kCreateCommandQueueX = 48;
inline constexpr std::size_t kSetFrameIntervalX = 78;
inline constexpr std::size_t kScheduleFrameEventX = 79;
inline constexpr std::size_t kSlots = 160;
}  // namespace device_slot

namespace queue_slot {
inline constexpr std::size_t kPcSlots = 18;
inline constexpr std::size_t kSlots = 64;
}  // namespace queue_slot

namespace list_slot {
inline constexpr std::size_t kPcSlots = 9;
inline constexpr std::size_t kSlots = 160;
}  // namespace list_slot

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
ConsoleHeap* wrap_heap(ConsoleDevice* device, ID3D12DescriptorHeap* pc);
ConsoleAdapter* wrap_adapter(IDXGIAdapter1* pc);
ConsoleDxgiDevice* wrap_dxgi_device(ConsoleDevice* device);

HRESULT STDMETHODCALLTYPE device_set_frame_interval(ConsoleDevice*, void* output, UINT interval_us, UINT periods,
                                                    UINT flags);
HRESULT STDMETHODCALLTYPE device_schedule_frame_event(ConsoleDevice*, UINT type, UINT offset_us,
                                                      const ConsoleObjectList* objects, UINT flags);

ULONG add_ref(GraphicsHead& head);
ULONG release_device(ConsoleDevice* device);

}  // namespace g6shim::d3d12
