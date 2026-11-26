// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/shadow.hpp"

#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace g6shim::d3d12_xs;

namespace {

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

struct HeapBytes {
    descriptor::HeapDesc desc;
    descriptor::Placement placement{};
};

void drop_object(Object* object) {
    if (object != nullptr) slot<std::uint32_t (*)(Object*)>(object, 0x10)(object);
}
#define release(object) drop_object(object)

Object* make_heap(std::uint32_t count) {
    HeapBytes bytes{{descriptor::kViewsKind, count, descriptor::kShaderVisible, 0}, {}};
    Object* heap = nullptr;
    if (descriptor::create_heap(nullptr, &bytes.desc, &descriptor::kIids[0], &heap) != S_OK) return nullptr;
    return heap;
}

Object* make_placed_heap(void* memory, std::uint32_t count) {
    const auto step = descriptor::increment(descriptor::kViewsKind);
    if (!step) return nullptr;
    HeapBytes bytes{{descriptor::kViewsKind, count, descriptor::kShaderVisible, 0},
                    {reinterpret_cast<std::uint64_t>(memory), static_cast<std::uint64_t>(*step) * count}};
    Object* heap = nullptr;
    if (descriptor::create_heap(nullptr, &bytes.desc, &descriptor::kIids[0], &heap) != S_OK) return nullptr;
    return heap;
}

Object* make_buffer(std::uint64_t width) {
    const resource::HeapProperties properties{1, 0, 0, 0, 0};
    resource::ResourceDesc desc{};
    desc.dimension = resource::kBuffer;
    desc.width = width;
    desc.height = 1;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 1;
    desc.sample_count = 1;
    desc.layout = 1;
    Object* resource = nullptr;
    if (resource::create_committed(nullptr, &properties, 0, &desc, 0, nullptr, &resource::kResourceIids[0],
                                   &resource) != S_OK)
        return nullptr;
    return resource;
}

std::uint32_t* title_words(Object* buffer) {
    const auto bytes = resource::buffer_bytes(buffer);
    return bytes ? reinterpret_cast<std::uint32_t*>(bytes->first) : nullptr;
}

void publish(Object* buffer) {
    const auto bytes = resource::buffer_bytes(buffer);
    if (!bytes) return;
    const auto window = shadow::find(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second);
    if (window) (*window)->update(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second);
}

void write_record(Object* resource, std::uint64_t handle, const D3D12_SHADER_RESOURCE_VIEW_DESC& srv) {
    std::array<std::uint32_t, 10> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    descriptor::create_srv(nullptr, resource, words.data(), handle);
}

std::array<std::uint8_t, 32> capture(Object* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srv) {
    alignas(16) std::array<std::uint8_t, 32> record{};
    write_record(resource, reinterpret_cast<std::uint64_t>(record.data()), srv);
    return record;
}

void plant(descriptor::Heap& heap, std::uint32_t index, const std::array<std::uint8_t, 32>& record) {
    const auto step = descriptor::increment(descriptor::kViewsKind).value_or(32);
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(heap.start + static_cast<std::uint64_t>(index) * step)),
                record.data(), record.size());
}

void sweep(host::Host* gpu, Object* heap) {
    const auto list = gpu->new_list();
    if (!list) return;
    list->note_descriptor_heap(heap);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
}

using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, ID3DInclude*, const char*,
                                   const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool compile_shader(const char* source, Microsoft::WRL::ComPtr<ID3DBlob>& code) {
    HMODULE library = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (library == nullptr) return false;
    const auto compile = reinterpret_cast<CompileFn>(::GetProcAddress(library, "D3DCompile"));
    if (compile == nullptr) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    return SUCCEEDED(compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                             &errors));
}

D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv(std::uint32_t first, std::uint32_t elements) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = first;
    srv.Buffer.NumElements = elements;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return srv;
}

}  // namespace

TEST(d3d12_xs_memcpy_record_is_visible_to_the_next_dispatch) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap_object = make_heap(4);
    Object* buffer = make_buffer(256);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    std::uint32_t* words = title_words(buffer);
    CHECK(words != nullptr);
    if (words == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    constexpr std::uint32_t kValue = 0xA1B2C3D4u;
    words[0] = kValue;
    auto* heap = static_cast<descriptor::Heap*>(heap_object);
    const auto record = capture(buffer, raw_srv(0, 1));
    plant(*heap, 0, record);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    const auto output = gpu->shared_buffer(16);
    CHECK(output.has_value());
    if (!output) {
        release(heap_object);
        release(buffer);
        return;
    }
    std::memset(output->second, 0, 16);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(output->first.Get(), nullptr, &uav, host_heap->cpu(1));
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    CHECK(compile_shader("cbuffer Off : register(b0) { uint offset; };"
                         "ByteAddressBuffer src : register(t0);"
                         "RWByteAddressBuffer dst : register(u0);"
                         "[numthreads(1,1,1)] void main() { dst.Store(offset, src.Load(0)); }",
                         code));
    if (!code) {
        release(heap_object);
        release(buffer);
        return;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 1;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    CHECK(SUCCEEDED(::D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (!blob || FAILED(gpu->device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&root)))) {
        release(heap_object);
        release(buffer);
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
    if (!pipeline) {
        release(heap_object);
        release(buffer);
        return;
    }
    const auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) {
        release(heap_object);
        release(buffer);
        return;
    }
    ID3D12GraphicsCommandList* commands = list->list();
    ID3D12DescriptorHeap* heaps[] = {host_heap->heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    commands->SetComputeRoot32BitConstant(0, 0, 0);
    const D3D12_GPU_DESCRIPTOR_HANDLE table{host_heap->gpu_start()};
    commands->SetComputeRootDescriptorTable(1, table);
    commands->Dispatch(1, 1, 1);
    list->note_descriptor_heap(heap_object);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
    CHECK(std::memcmp(output->second, &kValue, sizeof kValue) == 0);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap_object);
    release(buffer);
}

TEST(d3d12_xs_unchanged_record_is_not_rebuilt) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap_object = make_heap(4);
    Object* buffer = make_buffer(64);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    auto* heap = static_cast<descriptor::Heap*>(heap_object);
    plant(*heap, 1, capture(buffer, raw_srv(0, 1)));
    sweep(gpu, heap_object);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    const std::uint64_t rebuilt = host_heap->rebuilds();
    CHECK(rebuilt >= 1);
    ID3D12Resource* kept = host_heap->kept(1).Get();
    CHECK(kept != nullptr);
    sweep(gpu, heap_object);
    CHECK(host_heap->rebuilds() == rebuilt);
    CHECK(host_heap->kept(1).Get() == kept);
    release(heap_object);
    release(buffer);
}

TEST(d3d12_xs_buffer_record_rewritten_as_texture_changes_host_kind) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap_object = make_heap(2);
    Object* buffer = make_buffer(64);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    auto* heap = static_cast<descriptor::Heap*>(heap_object);
    plant(*heap, 0, capture(buffer, raw_srv(0, 1)));
    sweep(gpu, heap_object);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr && host_heap->kind_at(0) == 1);
    resource::ResourceDesc texture{};
    texture.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.width = 16;
    texture.height = 16;
    texture.depth_or_array_size = 1;
    texture.mip_levels = 1;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.sample_count = 1;
    const resource::HeapProperties properties{1, 0, 0, 1, 1};
    Object* image = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &texture, 0, nullptr, &resource::kResourceIids[0],
                                     &image) == S_OK);
    if (image != nullptr && host_heap != nullptr) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        plant(*heap, 0, capture(image, srv));
        sweep(gpu, heap_object);
        CHECK(host_heap->kind_at(0) == 2);
    }
    release(image);
    release(heap_object);
    release(buffer);
}

TEST(d3d12_xs_off_element_view_gets_its_own_copy) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap_object = make_heap(2);
    Object* buffer = make_buffer(256);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    const auto bytes = resource::buffer_bytes(buffer);
    CHECK(bytes.has_value());
    if (!bytes) {
        release(heap_object);
        release(buffer);
        return;
    }
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(bytes->first);
    const auto home = shadow::find(base, bytes->second);
    CHECK(home.has_value());
    std::array<std::uint32_t, 14> resource{};
    resource[0] = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource[4] = 56;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1;
    srv.Buffer.StructureByteStride = 56;
    alignas(16) std::array<std::uint8_t, 32> side{};
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    const std::uint64_t address = base + 8;
    words[10] = static_cast<std::uint32_t>(address);
    words[11] = static_cast<std::uint32_t>(address >> 32);
    descriptor::create_placed_srv(nullptr, resource.data(), words.data(),
                                  reinterpret_cast<std::uint64_t>(side.data()));
    plant(*static_cast<descriptor::Heap*>(heap_object), 0, side);
    sweep(gpu, heap_object);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr && home.has_value());
    if (host_heap != nullptr && home) {
        ID3D12Resource* kept = host_heap->kept(0).Get();
        CHECK(kept != nullptr);
        CHECK(kept != (*home)->buffer());
    }
    release(heap_object);
    release(buffer);
}

TEST(d3d12_xs_per_slot_stride_is_used_for_structured_access) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap_object = make_heap(4);
    Object* buffer = make_buffer(64);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    std::uint32_t* words = title_words(buffer);
    CHECK(words != nullptr);
    if (words == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    constexpr std::uint32_t kValue = 0x51u;
    words[4] = kValue;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 2;
    srv.Buffer.StructureByteStride = 16;
    auto* heap = static_cast<descriptor::Heap*>(heap_object);
    plant(*heap, 0, capture(buffer, srv));
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap_object);
        release(buffer);
        return;
    }
    const auto output = gpu->shared_buffer(16);
    CHECK(output.has_value());
    if (!output) {
        release(heap_object);
        release(buffer);
        return;
    }
    std::memset(output->second, 0, 16);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(output->first.Get(), nullptr, &uav, host_heap->cpu(1));
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    CHECK(compile_shader("struct Item { uint v; uint a; uint b; uint c; };"
                         "StructuredBuffer<Item> src : register(t0);"
                         "RWByteAddressBuffer dst : register(u0);"
                         "[numthreads(1,1,1)] void main() { dst.Store(0, src[1].v); }",
                         code));
    if (!code) {
        release(heap_object);
        release(buffer);
        return;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameters[1]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(::D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
        FAILED(gpu->device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root)))) {
        release(heap_object);
        release(buffer);
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
    if (!pipeline) {
        release(heap_object);
        release(buffer);
        return;
    }
    const auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) {
        release(heap_object);
        release(buffer);
        return;
    }
    ID3D12GraphicsCommandList* commands = list->list();
    ID3D12DescriptorHeap* heaps[] = {host_heap->heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE table{host_heap->gpu_start()};
    commands->SetComputeRootDescriptorTable(0, table);
    commands->Dispatch(1, 1, 1);
    list->note_descriptor_heap(heap_object);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
    CHECK(host_heap->stride_word(0) == 16);
    CHECK(std::memcmp(output->second, &kValue, sizeof kValue) == 0);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap_object);
    release(buffer);
}

TEST(d3d12_xs_write_watched_placed_heap_record_is_mirrored) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    constexpr std::uint32_t kCount = 4;
    const auto step = descriptor::increment(descriptor::kViewsKind);
    CHECK(step.has_value());
    if (!step) return;
    const std::size_t bytes = static_cast<std::size_t>(*step) * kCount;
    void* memory = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    Object* heap_object = make_placed_heap(memory, kCount);
    Object* buffer = make_buffer(64);
    CHECK(heap_object != nullptr && buffer != nullptr);
    if (heap_object == nullptr || buffer == nullptr) {
        release(heap_object);
        release(buffer);
        ::VirtualFree(memory, 0, MEM_RELEASE);
        return;
    }
    plant(*static_cast<descriptor::Heap*>(heap_object), 0, capture(buffer, raw_srv(0, 1)));
    sweep(gpu, heap_object);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap_object);
    CHECK(host_heap != nullptr && host_heap->kept(0).Get() != nullptr);
    if (host_heap != nullptr) {
        const std::uint64_t rebuilt = host_heap->rebuilds();
        sweep(gpu, heap_object);
        CHECK(host_heap->rebuilds() == rebuilt);
    }
    release(heap_object);
    release(buffer);
    ::VirtualFree(memory, 0, MEM_RELEASE);
}
