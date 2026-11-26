// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/shadow.hpp"
#include "d3d12_xs/texture.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace g6shim::d3d12_xs;


namespace {

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

struct LagGuard {
    LagGuard() { host::set_bury_lag_for_test(0); }
    ~LagGuard() {
        host::reset_bury_lag_for_test();
        host::pause_completion_for_test(false);
        if (host::host() != nullptr) host::wait_idle();
    }
};

host::Host* test_host() {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    return host::host();
}

resource::ResourceDesc texture_desc() {
    resource::ResourceDesc desc{};
    desc.dimension = 3;
    desc.width = 4;
    desc.height = 4;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 1;
    desc.format = 28;
    desc.sample_count = 1;
    desc.layout = 0;
    desc.flags = 1;
    return desc;
}

Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

struct QueueStall {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;

    explicit QueueStall(ID3D12CommandQueue* queue) {
        host::Host* gpu = host::host();
        if (gpu == nullptr || queue == nullptr || gpu->device() == nullptr) return;
        if (FAILED(gpu->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
        queue->Wait(fence.Get(), 1);
    }

    void release() {
        if (fence) fence->Signal(1);
    }

    ~QueueStall() { release(); }
};

}  // namespace

static void d3d12_xs_retirement_keeps_a_released_texture_until_the_list_completes() {
    LagGuard lag;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    const resource::HeapProperties properties{1, 0, 0, 0, 0};
    const resource::ResourceDesc desc = texture_desc();
    Object* title = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &desc, 4, nullptr, &resource::kResourceIids[0],
                                     &title) == S_OK);
    CHECK(title != nullptr);
    if (title == nullptr) return;
    ID3D12Resource* texture = texture::host_texture(title);
    CHECK(texture != nullptr);
    if (texture == nullptr) return;

    auto clearer = gpu->new_list();
    CHECK(clearer != nullptr);
    if (clearer == nullptr) return;
    const float red[4] = {1.f, 0.f, 0.f, 1.f};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu->rtv(texture, nullptr);
    clearer->list()->ClearRenderTargetView(rtv, red, 0, nullptr);
    clearer->close();
    gpu->execute({clearer.get()});
    host::wait_idle();
    clearer.reset();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC host_desc = texture->GetDesc();
    gpu->device()->GetCopyableFootprints(&host_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
    CHECK(total > 0);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback = readback_buffer(gpu->device(), total);
    CHECK(readback != nullptr);
    if (readback == nullptr) return;

    auto reader = gpu->new_list();
    CHECK(reader != nullptr);
    if (reader == nullptr) return;
    const D3D12_RESOURCE_BARRIER to_source =
        host::transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    reader->list()->ResourceBarrier(1, &to_source);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    reader->list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    reader->close();

    texture->AddRef();
    const std::uint64_t faults = host::free_during_submission_faults_for_test();
    const std::uint64_t retired = host::retired_count_for_test();
    QueueStall stall(gpu->queue());
    CHECK(stall.fence != nullptr);
    host::pause_completion_for_test(true);
    gpu->execute({reader.get()});
    CHECK(slot<std::uint32_t (*)(Object*)>(title, 0x10)(title) == 0);
    CHECK(host::submission_in_flight());
    CHECK(host::free_during_submission_faults_for_test() > faults);
    CHECK(host::retired_count_for_test() > retired);
    CHECK(host::references(texture) >= 2);
    stall.release();
    host::pause_completion_for_test(false);
    host::wait_idle();
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    CHECK(SUCCEEDED(readback->Map(0, &range, &mapped)));
    if (mapped != nullptr) {
        const auto* pixel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
        CHECK(pixel[0] == 255);
        CHECK(pixel[1] == 0);
        CHECK(pixel[2] == 0);
        CHECK(pixel[3] == 255);
        readback->Unmap(0, nullptr);
    }
    CHECK(host::references(texture) == 1);
    CHECK(texture->Release() == 0);
}

static void d3d12_xs_retirement_loose_window_eviction_does_not_free_an_in_flight_window() {
    LagGuard lag;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    constexpr std::size_t kRegion = 8 << 20;
    constexpr std::size_t kBytes = 16;
    auto* memory = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, kRegion, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    std::memset(memory, 0xAB, kBytes);
    const auto address = reinterpret_cast<std::uint64_t>(memory);
    auto window = shadow::find(address, kBytes);
    CHECK(window.has_value());
    if (!window) {
        ::VirtualFree(memory, 0, MEM_RELEASE);
        return;
    }
    CHECK((*window)->update(address, kBytes) == kBytes);
    ID3D12Resource* buffer = (*window)->buffer();
    CHECK(buffer != nullptr);
    const std::uint64_t offset = address - (*window)->start();
    Microsoft::WRL::ComPtr<ID3D12Resource> readback = readback_buffer(gpu->device(), kBytes);
    CHECK(readback != nullptr);
    if (readback == nullptr || buffer == nullptr) {
        ::VirtualFree(memory, 0, MEM_RELEASE);
        return;
    }
    auto reader = gpu->new_list();
    CHECK(reader != nullptr);
    if (reader == nullptr) {
        ::VirtualFree(memory, 0, MEM_RELEASE);
        return;
    }
    const D3D12_RESOURCE_BARRIER to_source =
        host::transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    reader->list()->ResourceBarrier(1, &to_source);
    reader->list()->CopyBufferRegion(readback.Get(), 0, buffer, offset, kBytes);
    reader->close();
    window.reset();
    QueueStall stall(gpu->queue());
    CHECK(stall.fence != nullptr);
    host::pause_completion_for_test(true);
    gpu->execute({reader.get()});
    CHECK(host::submission_in_flight());
    const std::uint64_t evicted = shadow::loose_stats().evicted;
    for (std::uint64_t i = 0; i <= shadow::kIdleSubmissions; ++i) shadow::submission_completed();
    CHECK(shadow::loose_stats().evicted > evicted);
    CHECK(!shadow::live_loose(buffer));
    CHECK(host::references(buffer) >= 1);
    buffer->AddRef();
    stall.release();
    host::pause_completion_for_test(false);
    host::wait_idle();
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    void* mapped = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    if (mapped != nullptr) {
        CHECK(static_cast<const std::uint8_t*>(mapped)[0] == 0xAB);
        CHECK(static_cast<const std::uint8_t*>(mapped)[kBytes - 1] == 0xAB);
        readback->Unmap(0, nullptr);
    }
    buffer->Release();
    ::VirtualFree(memory, 0, MEM_RELEASE);
}

static void d3d12_xs_retirement_drains_on_shutdown() {
    LagGuard lag;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    host::set_bury_lag_for_test(64);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    CHECK(SUCCEEDED(gpu->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&buffer))));
    if (buffer == nullptr) return;
    ID3D12Resource* raw = buffer.Get();
    raw->AddRef();
    const std::uint64_t before = host::retired_count_for_test();
    host::retire(std::move(buffer));
    CHECK(host::retired_count_for_test() == before + 1);
    CHECK(host::references(raw) >= 2);
    host::release_retired();
    CHECK(host::retired_count_for_test() == before + 1);
    host::drain_retired();
    CHECK(host::retired_count_for_test() == 0);
    CHECK(host::references(raw) == 1);
    CHECK(raw->Release() == 0);
}

void g6shim_retirement_tests_anchor() {
    g6shim::test::add("d3d12_xs_retirement_keeps_a_released_texture_until_the_list_completes",
                      &d3d12_xs_retirement_keeps_a_released_texture_until_the_list_completes);
    g6shim::test::add("d3d12_xs_retirement_loose_window_eviction_does_not_free_an_in_flight_window",
                      &d3d12_xs_retirement_loose_window_eviction_does_not_free_an_in_flight_window);
    g6shim::test::add("d3d12_xs_retirement_drains_on_shutdown", &d3d12_xs_retirement_drains_on_shutdown);
}
