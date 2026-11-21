// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "console_objects.hpp"

#include <dxgi1_4.h>

#include <cstdio>

using namespace g6shim::d3d12;

namespace {

struct WarpDevice {
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* pc = nullptr;
    ConsoleDevice* console = nullptr;

    WarpDevice() {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pc))))
            create_console_device(pc, adapter, &console);
        factory->Release();
    }
    ~WarpDevice() {
        if (console) release_device(console);
        if (pc) pc->Release();
        if (adapter) adapter->Release();
    }
    bool ready() const {
        if (!console) std::printf("  skipped: no WARP device\n");
        return console != nullptr;
    }
    template <class Fn>
    Fn slot(std::size_t index) const {
        return reinterpret_cast<Fn>(console->head.table[index]);
    }
};

template <class Fn>
Fn list_slot_fn(void* list, std::size_t index) {
    return reinterpret_cast<Fn>(static_cast<GraphicsHead*>(list)->table[index]);
}

using AllocationInfo = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(D3D12_RESOURCE_ALLOCATION_INFO*,
                                                                           ConsoleDevice*, UINT, UINT,
                                                                           const D3D12_RESOURCE_DESC*);
using HeapProperties = D3D12_HEAP_PROPERTIES*(STDMETHODCALLTYPE*)(D3D12_HEAP_PROPERTIES*, ConsoleDevice*, UINT,
                                                                  D3D12_HEAP_TYPE);
using AdapterLuid = LUID(STDMETHODCALLTYPE*)(ConsoleDevice*);
using CreateAllocator = HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, UINT, REFIID, void**);
using CreateList = HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, UINT, UINT, ID3D12CommandAllocator*,
                                               ID3D12PipelineState*, REFIID, void**);
using ListHresult = HRESULT(STDMETHODCALLTYPE*)(void*);
using ListType = UINT(STDMETHODCALLTYPE*)(void*);
using ListRelease = ULONG(STDMETHODCALLTYPE*)(void*);
using CopyTextureRegionX = void(STDMETHODCALLTYPE*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*, UINT);

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

UINT closed_list_type(const WarpDevice& device, UINT console_type, UINT* type) {
    ID3D12CommandAllocator* allocator = nullptr;
    if (FAILED(device.slot<CreateAllocator>(device_slot::kCreateCommandAllocator)(device.console, console_type,
                                                                                  IID_PPV_ARGS(&allocator))))
        return 0;
    void* list = nullptr;
    UINT closes = 0;
    if (SUCCEEDED(device.slot<CreateList>(device_slot::kCreateCommandList)(
            device.console, 0, console_type, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), &list))) {
        *type = list_slot_fn<ListType>(list, list_slot::kGetType)(list);
        if (list_slot_fn<ListHresult>(list, pc_list_slot::kClose)(list) == S_OK) ++closes;
        if (FAILED(list_slot_fn<ListHresult>(list, pc_list_slot::kClose)(list))) ++closes;
        list_slot_fn<ListRelease>(list, kReleaseSlot)(list);
    }
    allocator->Release();
    return closes;
}

}  // namespace

TEST(console_device_returns_structures_the_console_way) {
    WarpDevice device;
    if (!device.ready()) return;
    const D3D12_RESOURCE_DESC desc = buffer_desc(65536, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    const D3D12_RESOURCE_DESC pc_desc = pc_resource_desc(desc);
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    D3D12_RESOURCE_ALLOCATION_INFO* returned =
        device.slot<AllocationInfo>(device_slot::kGetResourceAllocationInfo)(&info, device.console, 0, 1, &desc);
    const D3D12_RESOURCE_ALLOCATION_INFO expected = device.pc->GetResourceAllocationInfo(0, 1, &pc_desc);
    CHECK(returned == &info);
    CHECK(info.SizeInBytes == expected.SizeInBytes && info.Alignment == expected.Alignment);

    D3D12_HEAP_PROPERTIES heap{};
    device.slot<HeapProperties>(device_slot::kGetCustomHeapProperties)(&heap, device.console, 0,
                                                                        D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES expected_heap = device.pc->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD);
    CHECK(heap.Type == expected_heap.Type && heap.CPUPageProperty == expected_heap.CPUPageProperty &&
          heap.MemoryPoolPreference == expected_heap.MemoryPoolPreference);

    const LUID luid = device.slot<AdapterLuid>(device_slot::kGetAdapterLuid)(device.console);
    const LUID expected_luid = device.pc->GetAdapterLuid();
    CHECK(luid.LowPart == expected_luid.LowPart && luid.HighPart == expected_luid.HighPart);
}

TEST(console_lists_close_through_the_console_slots) {
    WarpDevice device;
    if (!device.ready()) return;
    UINT type = ~0u;
    CHECK(closed_list_type(device, kConsoleQueueTypeDirect, &type) == 2);
    CHECK(type == kConsoleQueueTypeDirect);
    type = ~0u;
    CHECK(closed_list_type(device, kConsoleQueueTypeDma, &type) == 2);
    CHECK(type == kConsoleQueueTypeDma);
}

TEST(console_lists_copy_texture_regions_with_flags) {
    WarpDevice device;
    if (!device.ready()) return;
    D3D12_RESOURCE_DESC texture_desc = buffer_desc(4, D3D12_RESOURCE_FLAG_NONE);
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Height = 4;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device.pc->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC upload_desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    void* list = nullptr;
    const bool created =
        SUCCEEDED(device.pc->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))) &&
        SUCCEEDED(device.pc->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(device.pc->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device.slot<CreateList>(device_slot::kCreateCommandList)(
            device.console, 0, kConsoleQueueTypeDirect, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), &list));
    CHECK(created);
    if (created) {
        D3D12_TEXTURE_COPY_LOCATION destination{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        source.PlacedFootprint = footprint;
        list_slot_fn<CopyTextureRegionX>(list, list_slot::kCopyTextureRegionX)(list, &destination, 0, 0, 0, &source,
                                                                               nullptr, 0);
        CHECK(list_slot_fn<ListHresult>(list, pc_list_slot::kClose)(list) == S_OK);
    }
    if (list) list_slot_fn<ListRelease>(list, kReleaseSlot)(list);
    if (allocator) allocator->Release();
    if (upload) upload->Release();
    if (texture) texture->Release();
}
