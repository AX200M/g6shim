// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kThreads = 4;
constexpr std::uint32_t kBelow = 2;
constexpr std::uint32_t kWider = 3;
using namespace g6shim::shader;
using namespace g6shim::shader::emit;
using namespace g6shim::test::lowering;

struct Fixture {
    const char* name;
    std::vector<std::uint32_t> words;
};

const Fixture kFixtures[] = {
#include "gfx10_fixtures.inc"
};

constexpr std::uint32_t kDwordsPerThread = 8;
constexpr std::uint32_t kBallot = 0;
constexpr std::uint32_t kCount = 1;
constexpr std::uint32_t kPrefix = 2;
constexpr std::uint32_t kElsewhere = 3;
constexpr std::uint32_t kWave = 4;
constexpr std::uint16_t kVgprs = 8;
constexpr std::uint32_t kUserSgprs = 5;
constexpr std::uint32_t kUserSgprShift = 1;

const Fixture* fixture_named(const char* name) {
    for (const Fixture& fixture : kFixtures)
        if (std::strcmp(fixture.name, name) == 0) return &fixture;
    return nullptr;
}

Lowered lower_mask(const std::vector<std::uint32_t>& words, std::array<std::uint32_t, 3> threads) {
    return lower_compute(words, kVgprs,
                         {root_param(RootType::Kind::Constants, 0, 0),
                          root_param(RootType::Kind::Table, 4, 1)},
                         {used_uav(4, 1)}, kUserSgprs << kUserSgprShift, threads);
}

std::uint32_t low_bits(std::uint32_t n) {
    if (n >= 32) return 0xFFFFFFFFu;
    if (n == 0) return 0;
    return (1u << n) - 1u;
}

ID3D12Device* warp_device() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return nullptr;
    return device;
}

bool run_mask(ID3D12Device* device, const std::vector<std::uint8_t>& dxil,
              std::vector<std::uint32_t>* out) {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 1;
    signature.pParameters = &parameter;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &error)))
        return false;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.CS.pShaderBytecode = dxil.data();
    pipeline.CS.BytecodeLength = dxil.size();
    ComPtr<ID3D12PipelineState> state;
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&state)))) return false;

    const std::uint32_t dwords = kThreads * kDwordsPerThread;
    const std::uint32_t bytes = dwords * 4;
    D3D12_HEAP_PROPERTIES heap_default{};
    heap_default.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES heap_upload{};
    heap_upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES heap_readback{};
    heap_readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> uav;
    if (FAILED(device->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&uav))))
        return false;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    buffer.Width = 256;
    ComPtr<ID3D12Resource> constants;
    if (FAILED(device->CreateCommittedResource(&heap_upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&constants))))
        return false;
    void* mapped = nullptr;
    if (FAILED(constants->Map(0, nullptr, &mapped))) return false;
    auto* words = static_cast<std::uint32_t*>(mapped);
    std::fill(words, words + 64, 0u);
    words[0] = kBelow;
    words[1] = kWider;
    constants->Unmap(0, nullptr);
    buffer.Width = bytes;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&heap_readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heaps{};
    heaps.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heaps.NumDescriptors = 1;
    heaps.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    if (FAILED(device->CreateDescriptorHeap(&heaps, IID_PPV_ARGS(&descriptors)))) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = dwords;
    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(uav.Get(), nullptr, &view,
                                      descriptors->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        return false;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return false;
    D3D12_RESOURCE_BARRIER to_uav{};
    to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav.Transition.pResource = uav.Get();
    to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_uav);
    ID3D12DescriptorHeap* heap = descriptors.Get();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(state.Get());
    list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = uav.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(readback.Get(), uav.Get());
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    if (FAILED(queue->Signal(fence.Get(), 1))) return false;
    if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return false;
        const HRESULT waited = fence->SetEventOnCompletion(1, event);
        if (SUCCEEDED(waited)) WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (FAILED(waited) || fence->GetCompletedValue() < 1) return false;
    }
    void* back = nullptr;
    if (FAILED(readback->Map(0, nullptr, &back))) return false;
    out->assign(dwords, 0);
    std::memcpy(out->data(), back, bytes);
    readback->Unmap(0, nullptr);
    return true;
}

}  // namespace

TEST(a_whole_quad_lane_mask_read_as_data_is_refused) {
    const std::vector<std::uint32_t> words = {
        0xBE850380u, 0xF4080202u, 0xFA000000u, 0xBF8CC07Fu, 0x7E020200u, 0xD4C50006u,
        0x00010101u, 0xBE870906u, 0x7E000207u, 0xE0700000u, 0x80020000u, 0xBF810000u};
    bool refused = false;
    std::string reason;
    try {
        (void)lower_mask(words, {1, 1, 1});
    } catch (const std::exception& error) {
        refused = true;
        reason = error.what();
    }
    CHECK(refused);
    CHECK(reason.find("a whole-quad lane mask read as data") != std::string::npos);
}

TEST(lane_masks_read_as_data_match_the_wave) {
    const Fixture* fixture = fixture_named("lane_mask_data");
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    Lowered lowered;
    bool translated = true;
    try {
        _putenv("G6SHIM_HOST_WAVE=1");
        lowered = lower_mask(fixture->words, {kThreads, 1, 1});
        _putenv("G6SHIM_HOST_WAVE=");
    } catch (const std::exception& error) {
        _putenv("G6SHIM_HOST_WAVE=");
        std::printf("lane mask program did not translate: %s\n", error.what());
        translated = false;
    }
    CHECK(translated);
    if (!translated) return;
    check_dxil(lowered.dxil, "lane_mask_data");
    ComPtr<ID3D12Device> device(warp_device());
    if (!device) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    std::vector<std::uint32_t> got;
    const bool ran = run_mask(device.Get(), lowered.dxil, &got);
    CHECK(ran);
    if (!ran || got.size() < kDwordsPerThread) return;
    const std::uint32_t wave = got[kWave];
    CHECK(wave > 0 && wave <= kThreads);
    if (wave == 0 || wave > kThreads) return;
    const std::uint32_t ballot = low_bits(std::min(kBelow, wave));
    const std::uint32_t elsewhere = low_bits(std::min(kWider, wave));
    for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
        const std::uint32_t lane = thread % wave;
        const std::uint32_t* row = got.data() + thread * kDwordsPerThread;
        CHECK(row[kBallot] == ballot);
        CHECK(row[kCount] == std::min(kBelow, wave));
        CHECK(row[kPrefix] == std::min(lane, kBelow));
        CHECK(row[kElsewhere] == elsewhere);
        CHECK(row[kWave] == wave);
    }
}
