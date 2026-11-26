// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstring>
#include <wrl/client.h>
#include <iterator>
#include <vector>

#include "g6shim/shader/dxbc.hpp"

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;
using namespace g6shim::test::lowering;

struct Fixture {
    const char* name;
    std::vector<std::uint32_t> words;
};

const Fixture kFixtures[] = {
#include "image64_fixtures.inc"
};

constexpr std::uint16_t kVgprs = 8;
constexpr std::uint8_t kConstantsUserData = 0;
constexpr std::uint8_t kTableUserData = 4;
constexpr std::uint8_t kImageDwords = 8;
constexpr std::uint32_t kUserSgprs = 5;
constexpr std::uint32_t kUserSgprShift = 1;
constexpr std::uint64_t kInt64Ops = 0x8000;
constexpr std::uint64_t kTypedAtomic64 = 0x400000;
constexpr std::uint64_t kHeapAtomic64 = 0x10000000;
constexpr std::uint64_t kResourceHeap = 0x2000000;
constexpr std::uint64_t kAtomic64Features = kInt64Ops | kTypedAtomic64 | kHeapAtomic64 | kResourceHeap;
constexpr std::size_t kFixtureCount = 14;

Lowered lower_image(const Fixture& fixture) {
    return lower_compute(fixture.words, kVgprs,
                         {root_param(RootType::Kind::Constants, kConstantsUserData, 0),
                          root_param(RootType::Kind::Table, kTableUserData, 1)},
                         {used_uav(kTableUserData, 1, kImageDwords)}, kUserSgprs << kUserSgprShift,
                         {1, 1, 1});
}

std::uint64_t feature_bits(const std::vector<std::uint8_t>& dxil) {
    const Container container = Container::parse(dxil);
    const Part* sfi = container.part(kSfi0);
    if (sfi == nullptr || sfi->data.size() < 8) return 0;
    std::uint64_t features = 0;
    for (std::size_t i = 0; i < 8; ++i)
        features |= static_cast<std::uint64_t>(sfi->data[i]) << (8 * i);
    return features;
}

}  // namespace

TEST(shader_image_atomic64_translates_and_validates) {
    CHECK(std::size(kFixtures) == kFixtureCount);
    for (const Fixture& fixture : kFixtures) {
        bool translated = true;
        Lowered lowered;
        try {
            lowered = lower_image(fixture);
        } catch (const std::exception& error) {
            std::printf("image atomic %s did not translate: %s\n", fixture.name, error.what());
            translated = false;
        }
        CHECK(translated);
        if (!translated) continue;
        CHECK((feature_bits(lowered.dxil) & kAtomic64Features) == kAtomic64Features);
        check_dxil(lowered.dxil, fixture.name);
    }
}

using Microsoft::WRL::ComPtr;

bool typed_atomic64(ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11{};
    const bool typed =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof options9)) &&
        options9.AtomicInt64OnTypedResourceSupported;
    const bool heap =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof options11)) &&
        options11.AtomicInt64OnDescriptorHeapResourceSupported;
    return typed && heap;
}

ID3D12Device* warp_device() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf())))) return nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return nullptr;
    return device;
}

TEST(shader_image_atomic64_adds_on_warp) {
    ComPtr<ID3D12Device> device;
    device.Attach(warp_device());
    if (!device || !typed_atomic64(device.Get())) {
        std::printf("skipped: host has no 64-bit typed UAV atomics\n");
        return;
    }
    const Fixture* add = nullptr;
    for (const Fixture& fixture : kFixtures)
        if (std::strcmp(fixture.name, "image_atomic_add") == 0) add = &fixture;
    CHECK(add != nullptr);
    if (add == nullptr) return;
    Lowered lowered;
    try {
        lowered = lower_image(*add);
    } catch (const std::exception& error) {
        std::printf("image atomic add did not translate: %s\n", error.what());
        CHECK(false);
        return;
    }
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    const HRESULT serialized_hr = D3D12SerializeRootSignature(
        &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(), error.GetAddressOf());
    CHECK(SUCCEEDED(serialized_hr));
    if (FAILED(serialized_hr)) return;
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                IID_PPV_ARGS(root.GetAddressOf()))));
    if (!root) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS.pShaderBytecode = lowered.dxil.data();
    pipeline_desc.CS.BytecodeLength = lowered.dxil.size();
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT pipeline_hr = device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(pipeline_hr)) std::printf("pipeline refused %08x\n", static_cast<unsigned>(pipeline_hr));
    CHECK(SUCCEEDED(pipeline_hr));
    if (!pipeline) return;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf()))));
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> clear_heap;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(clear_heap.GetAddressOf()))));
    if (!heap || !clear_heap) return;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 1;
    texture_desc.Height = 1;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R32G32_UINT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> texture;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    IID_PPV_ARGS(texture.GetAddressOf()))));
    if (!texture) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32_UINT;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, heap->GetCPUDescriptorHandleForHeapStart());
    device->CreateUnorderedAccessView(texture.Get(), nullptr, &view,
                                      clear_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC constants_desc{};
    constants_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constants_desc.Width = 256;
    constants_desc.Height = 1;
    constants_desc.DepthOrArraySize = 1;
    constants_desc.MipLevels = 1;
    constants_desc.SampleDesc.Count = 1;
    constants_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> constants;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &constants_desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(constants.GetAddressOf()))));
    if (!constants) return;
    void* mapped = nullptr;
    CHECK(SUCCEEDED(constants->Map(0, nullptr, &mapped)));
    if (mapped == nullptr) return;
    auto* words = static_cast<std::uint32_t*>(mapped);
    words[0] = 0;
    words[1] = 0;
    words[2] = 1;
    words[3] = 0;
    words[4] = 0;
    for (const auto& field : lowered.user_data) {
        if (field.source == g6shim::shader::emit::UserData::Source::ViewHeapCount && field.index < 64)
            words[field.index] = 1;
    }
    constants->Unmap(0, nullptr);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC read_desc = constants_desc;
    read_desc.Width = footprint.Footprint.RowPitch;
    ComPtr<ID3D12Resource> read;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &read_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(read.GetAddressOf()))));
    if (!read) return;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf()))));
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(allocator.GetAddressOf()))));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                              IID_PPV_ARGS(list.GetAddressOf()))));
    if (!queue || !allocator || !list) return;
    const UINT zeros[4] = {};
    list->ClearUnorderedAccessViewUint(heap->GetGPUDescriptorHandleForHeapStart(),
                                       clear_heap->GetCPUDescriptorHandleForHeapStart(), texture.Get(), zeros, 0,
                                       nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = texture.Get();
    list->ResourceBarrier(1, &barrier);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    list->ResourceBarrier(1, &barrier);
    D3D12_RESOURCE_BARRIER copy_barrier{};
    copy_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_barrier.Transition.pResource = texture.Get();
    copy_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    copy_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &copy_barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = read.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))));
    if (!fence) return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event != nullptr);
    if (event == nullptr) return;
    CHECK(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    if (fence->GetCompletedValue() < 1) {
        CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
        WaitForSingleObject(event, 5000);
    }
    CloseHandle(event);
    void* out = nullptr;
    CHECK(SUCCEEDED(read->Map(0, nullptr, &out)));
    if (out == nullptr) return;
    const auto* result = static_cast<const std::uint32_t*>(out);
    if (result[0] != 1 || result[1] != 0)
        std::printf("warp add wrote %08x %08x\n", result[0], result[1]);
    CHECK(result[0] == 1 && result[1] == 0);
    read->Unmap(0, nullptr);
}
