// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "console_objects.hpp"
#include "constant_views.hpp"
#include "removal.hpp"
#include "texture_shadow.hpp"

#include <dxgi1_4.h>
#include <g6shim/window.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

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
using FlushPipeline = void(STDMETHODCALLTYPE*)(void*, UINT, UINT64, UINT64);
using CreateQueue = HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateCommitted = HRESULT(STDMETHODCALLTYPE*)(ConsoleDevice*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE*, REFIID, void**);
using ExecuteLists = void(STDMETHODCALLTYPE*)(void*, UINT, void* const*);
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
        if (console_type == kConsoleQueueTypeDirect)
            list_slot_fn<FlushPipeline>(list, list_slot::kFlushPipelineX)(list, 0x20000, 0x13820000, 0x40000);
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

namespace {

constexpr UINT kShadowTestSize = 8;
constexpr UINT kShadowTestPitch = 256;

std::uint8_t shadow_test_byte(UINT x, UINT y, UINT channel) {
    return static_cast<std::uint8_t>(x * 16 + y * 4 + channel);
}

bool wait_idle(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    queue->Signal(fence, 1);
    const bool done = SUCCEEDED(fence->SetEventOnCompletion(1, nullptr));
    fence->Release();
    return done;
}

bool texture_matches_pattern(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture) {
    const D3D12_RESOURCE_DESC texture_desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    bool matches = false;
    if (SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                            IID_PPV_ARGS(&list)))) {
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        barrier.Transition = {texture, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION destination{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = 0;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        void* data = nullptr;
        if (wait_idle(device, queue) && SUCCEEDED(readback->Map(0, nullptr, &data))) {
            matches = true;
            const auto* bytes_read = static_cast<const std::uint8_t*>(data);
            for (UINT y = 0; y < kShadowTestSize; ++y)
                for (UINT x = 0; x < kShadowTestSize; ++x)
                    for (UINT c = 0; c < 4; ++c)
                        if (bytes_read[y * footprint.Footprint.RowPitch + x * 4 + c] != shadow_test_byte(x, y, c))
                            matches = false;
            readback->Unmap(0, nullptr);
        }
    }
    if (list) list->Release();
    if (allocator) allocator->Release();
    if (readback) readback->Release();
    return matches;
}

}  // namespace

TEST(console_textures_take_cpu_writes_through_their_address) {
    WarpDevice device;
    if (!device.ready()) return;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void* queue = nullptr;
    CHECK(device.slot<CreateQueue>(device_slot::kCreateCommandQueue)(device.console, &queue_desc,
                                                                     __uuidof(ID3D12CommandQueue), &queue) == S_OK);
    D3D12_RESOURCE_DESC desc = buffer_desc(kShadowTestSize, D3D12_RESOURCE_FLAG_NONE);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Height = kShadowTestSize;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* texture = nullptr;
    CHECK(device.slot<CreateCommitted>(device_slot::kCreateCommittedResource)(
              device.console, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture)) == S_OK);
    if (!queue || !texture) return;
    auto* texels = reinterpret_cast<std::uint8_t*>(texture->GetGPUVirtualAddress());
    CHECK(texels != nullptr);
    if (texels) {
        for (UINT y = 0; y < kShadowTestSize; ++y)
            for (UINT x = 0; x < kShadowTestSize; ++x)
                for (UINT c = 0; c < 4; ++c) texels[y * kShadowTestPitch + x * 4 + c] = shadow_test_byte(x, y, c);
        list_slot_fn<ExecuteLists>(queue, queue_slot::kExecuteCommandLists)(queue, 0, nullptr);
        CHECK(texture_matches_pattern(device.pc, static_cast<ConsoleQueue*>(queue)->pc, texture));
    }
    CHECK(texture->Release() == 0);
    list_slot_fn<ListRelease>(queue, kReleaseSlot)(queue);
}

TEST(texture_shadows_copy_every_subresource) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 32;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = 0;
    desc.Format = DXGI_FORMAT_BC1_UNORM;
    desc.SampleDesc.Count = 1;
    std::vector<ShadowCopy> copies;
    UINT64 bytes = 0;
    CHECK(shadow_copies(desc, &copies, &bytes));
    CHECK(copies.size() == 14);
    if (copies.size() != 14) return;
    CHECK(copies[0].subresource == 0 && copies[0].footprint.Offset == 0 && copies[0].footprint.Footprint.RowPitch == 256);
    CHECK(copies[7].subresource == 7 && copies[7].footprint.Offset == copies[0].footprint.Offset + 2048);
    CHECK(copies[6].footprint.Footprint.Width == 4 && copies[6].footprint.Footprint.Height == 4);
    CHECK(bytes >= copies[13].footprint.Offset + 256);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    CHECK(!shadow_copies(desc, &copies, &bytes));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.SampleDesc.Count = 4;
    CHECK(!shadow_copies(desc, &copies, &bytes));
}

TEST(healthy_devices_report_no_removal) {
    WarpDevice device;
    if (!device.ready()) return;
    CHECK(!report_device_removal(device.pc));
}

TEST(patched_tables_copy_only_readable_slots) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(page != nullptr);
    if (!page) return;
    const auto* table = static_cast<const void* const*>(page);
    CHECK(readable_slots(table, resource_slot::kCopiedSlots) == resource_slot::kCopiedSlots);
    CHECK(readable_slots(table + 500, resource_slot::kCopiedSlots) == 12);
    VirtualFree(page, 0, MEM_RELEASE);
    CHECK(readable_slots(table, resource_slot::kCopiedSlots) == 0);
}

TEST(console_queues_present_a_render_target) {
    WarpDevice device;
    if (!device.ready()) return;
    const HWND window = CreateWindowExW(0, L"STATIC", L"g6shim present test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                        nullptr, nullptr, nullptr, nullptr);
    CHECK(window != nullptr);
    if (!window) return;
    g6shim::set_main_window(window);
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void* queue = nullptr;
    CHECK(device.slot<CreateQueue>(device_slot::kCreateCommandQueue)(device.console, &queue_desc,
                                                                     __uuidof(ID3D12CommandQueue), &queue) == S_OK);
    D3D12_RESOURCE_DESC desc = buffer_desc(64, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Height = 32;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* back_buffer = nullptr;
    CHECK(SUCCEEDED(device.pc->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&back_buffer))));
    if (queue && back_buffer) {
        using PresentX = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, const ConsolePresentPlane*, const void*);
        ConsolePresentPlane plane{1, 1, 0, &back_buffer};
        for (int frame = 0; frame < 4; ++frame) {
            plane.token = static_cast<std::uint64_t>(frame) + 1;
            CHECK(list_slot_fn<PresentX>(queue, queue_slot::kPresentX)(queue, 1, &plane, nullptr) == S_OK);
        }
        CHECK(list_slot_fn<PresentX>(queue, queue_slot::kPresentX)(queue, 0, &plane, nullptr) == E_INVALIDARG);
        CHECK(wait_idle(device.pc, static_cast<ConsoleQueue*>(queue)->pc));
        CHECK(!report_device_removal(device.pc));
    }
    if (back_buffer) back_buffer->Release();
    if (queue) list_slot_fn<ListRelease>(queue, kReleaseSlot)(queue);
    g6shim::set_main_window(nullptr);
    DestroyWindow(window);
}

TEST(constant_views_fit_pc_alignment) {
    CHECK(constant_view_fits_pc(0x10000, 256));
    CHECK(!constant_view_fits_pc(0x10010, 256));
    CHECK(!constant_view_fits_pc(0x10000, 96));
    CHECK(pc_constant_view_size(96) == 256 && pc_constant_view_size(368) == 512 && pc_constant_view_size(256) == 256);
}

TEST(unaligned_constant_views_follow_their_source) {
    WarpDevice device;
    if (!device.ready()) return;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC desc = buffer_desc(65536, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* ring = nullptr;
    CHECK(device.slot<CreateCommitted>(device_slot::kCreateCommittedResource)(
              device.console, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
              __uuidof(ID3D12Resource), reinterpret_cast<void**>(&ring)) == S_OK);
    if (!ring) return;
    void* mapped = nullptr;
    CHECK(SUCCEEDED(ring->Map(0, nullptr, &mapped)));
    auto* constants = static_cast<float*>(mapped) + 4;
    constants[0] = 1.5f;
    UINT64 relocated = 0;
    CHECK(relocate_constants(device.pc, ring->GetGPUVirtualAddress() + 16, 96, &relocated));
    CHECK(relocated % kConstantViewAlignment == 0);
    const auto* copy = static_cast<const float*>(relocated_constants(relocated));
    CHECK(copy && copy[0] == 1.5f);
    constants[0] = 2.5f;
    refresh_relocated_constants();
    CHECK(copy && copy[0] == 2.5f);
    UINT64 again = 0;
    CHECK(relocate_constants(device.pc, ring->GetGPUVirtualAddress() + 16, 96, &again) && again == relocated);
    CHECK(!relocate_constants(device.pc, 0x10, 96, &again));
    ring->Release();
}

TEST(console_devices_send_every_call_through_their_tables) {
    WarpDevice device;
    if (!device.ready()) return;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(device.console);
    CHECK(bytes[kDeviceOutOfLineFlagsOffset] == kEveryCallOutOfLine);
}
