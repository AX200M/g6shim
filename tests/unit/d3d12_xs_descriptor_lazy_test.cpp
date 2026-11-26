// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/shadow.hpp"

#include <g6shim/shader/emit/emit.hpp>

#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

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

struct LazyOff {
    LazyOff() {
        ::SetEnvironmentVariableA("G6SHIM_LAZY_DESCRIPTORS", "0");
        descriptor_host::refresh_descriptor_settings();
    }
    ~LazyOff() {
        ::SetEnvironmentVariableA("G6SHIM_LAZY_DESCRIPTORS", nullptr);
        descriptor_host::refresh_descriptor_settings();
    }
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

void write_raw_srv(Object* buffer, std::uint64_t handle, std::uint32_t elements) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = elements;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    std::array<std::uint32_t, 10> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    descriptor::create_srv(nullptr, buffer, words.data(), handle);
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

using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, ID3DInclude*, const char*,
                                   const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct ShaderRead {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    descriptor_host::HostHeap* heap = nullptr;
};

std::optional<ShaderRead> shader_reader(host::Host* gpu, descriptor_host::HostHeap* heap, ID3D12Resource* output) {
    HMODULE library = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (library == nullptr) return std::nullopt;
    const auto compile = reinterpret_cast<CompileFn>(::GetProcAddress(library, "D3DCompile"));
    if (compile == nullptr) return std::nullopt;
    const char* source =
        "cbuffer Off : register(b0) { uint offset; };"
        "ByteAddressBuffer src : register(t0);"
        "RWByteAddressBuffer dst : register(u0);"
        "[numthreads(1,1,1)] void main() { dst.Store(offset, src.Load(0)); }";
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                       &errors)))
        return std::nullopt;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 1;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (FAILED(::D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
        return std::nullopt;
    ShaderRead reader;
    reader.heap = heap;
    if (FAILED(gpu->device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&reader.root))))
        return std::nullopt;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(output, nullptr, &uav, heap->cpu(1));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = reader.root.Get();
    pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(gpu->device()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&reader.pipeline))))
        return std::nullopt;
    return reader;
}

bool dispatch_read(host::Host* gpu, const ShaderRead& reader, std::uint32_t offset) {
    const auto list = gpu->new_list();
    if (!list) return false;
    ID3D12GraphicsCommandList* commands = list->list();
    ID3D12DescriptorHeap* heaps[] = {reader.heap->heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(reader.root.Get());
    commands->SetPipelineState(reader.pipeline.Get());
    commands->SetComputeRoot32BitConstant(0, offset, 0);
    const D3D12_GPU_DESCRIPTOR_HANDLE table{reader.heap->gpu_start()};
    commands->SetComputeRootDescriptorTable(1, table);
    commands->Dispatch(1, 1, 1);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
    return true;
}

}  // namespace

TEST(d3d12_xs_lazy_draw_reads_the_descriptor_written_before_it) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    CHECK(descriptor_host::lazy_descriptors());
    Object* heap = make_heap(4);
    Object* buffer = make_buffer(256);
    CHECK(heap != nullptr && buffer != nullptr);
    if (heap == nullptr || buffer == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    std::uint32_t* words = title_words(buffer);
    CHECK(words != nullptr);
    if (words == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    constexpr std::uint32_t kValue = 0xA1B2C3D4u;
    words[0] = kValue;
    const std::uint64_t handle = static_cast<descriptor::Heap*>(heap)->start;
    descriptor_host::take_descriptor_stats();
    write_raw_srv(buffer, handle, 1);
    const descriptor_host::DescriptorStats marked = descriptor_host::take_descriptor_stats();
    CHECK(marked.marked == 1);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    CHECK(host_heap->kept(0).Get() == nullptr);
    const auto view = descriptor_host::materialize_for_draw(heap, 0, handle);
    CHECK(view.has_value());
    CHECK(std::holds_alternative<descriptor::ShaderView>(*view));
    CHECK(host_heap->kept(0).Get() != nullptr);
    publish(buffer);
    const auto output = gpu->shared_buffer(16);
    CHECK(output.has_value());
    if (!output) {
        release(heap);
        release(buffer);
        return;
    }
    std::memset(output->second, 0, 16);
    const auto reader = shader_reader(gpu, host_heap, output->first.Get());
    CHECK(reader.has_value());
    if (!reader) {
        release(heap);
        release(buffer);
        return;
    }
    CHECK(dispatch_read(gpu, *reader, 0));
    CHECK(std::memcmp(output->second, &kValue, sizeof kValue) == 0);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap);
    release(buffer);
}

TEST(d3d12_xs_lazy_rewrite_between_draws_is_read_at_execution) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap = make_heap(4);
    Object* first = make_buffer(256);
    Object* second = make_buffer(256);
    CHECK(heap != nullptr && first != nullptr && second != nullptr);
    if (heap == nullptr || first == nullptr || second == nullptr) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    std::uint32_t* early = title_words(first);
    std::uint32_t* later = title_words(second);
    CHECK(early != nullptr && later != nullptr);
    if (early == nullptr || later == nullptr) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    constexpr std::uint32_t kEarly = 11;
    constexpr std::uint32_t kLater = 22;
    early[0] = kEarly;
    later[0] = kLater;
    const std::uint64_t handle = static_cast<descriptor::Heap*>(heap)->start;
    write_raw_srv(first, handle, 1);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    CHECK(descriptor_host::materialize_for_draw(heap, 0, handle).has_value());
    ID3D12Resource* early_view = host_heap->kept(0).Get();
    CHECK(early_view != nullptr);
    write_raw_srv(second, handle, 1);
    CHECK(descriptor_host::materialize_for_draw(heap, 0, handle).has_value());
    ID3D12Resource* later_view = host_heap->kept(0).Get();
    CHECK(later_view != nullptr);
    CHECK(later_view != early_view);
    publish(first);
    publish(second);
    const auto output = gpu->shared_buffer(16);
    CHECK(output.has_value());
    if (!output) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    std::memset(output->second, 0, 16);
    const auto reader = shader_reader(gpu, host_heap, output->first.Get());
    CHECK(reader.has_value());
    if (!reader) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    const auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) {
        release(heap);
        release(first);
        release(second);
        return;
    }
    ID3D12GraphicsCommandList* commands = list->list();
    ID3D12DescriptorHeap* heaps[] = {host_heap->heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(reader->root.Get());
    commands->SetPipelineState(reader->pipeline.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE table{host_heap->gpu_start()};
    commands->SetComputeRootDescriptorTable(1, table);
    commands->SetComputeRoot32BitConstant(0, 0, 0);
    commands->Dispatch(1, 1, 1);
    commands->SetComputeRoot32BitConstant(0, 4, 0);
    commands->Dispatch(1, 1, 1);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
    const auto* read = reinterpret_cast<const std::uint32_t*>(output->second);
    CHECK(read[0] == kLater);
    CHECK(read[1] == kLater);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap);
    release(first);
    release(second);
}

TEST(d3d12_xs_lazy_descriptor_copies_materialize_the_destination_range) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Object* heap = make_heap(8);
    Object* buffer = make_buffer(256);
    CHECK(heap != nullptr && buffer != nullptr);
    if (heap == nullptr || buffer == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    const auto* title = static_cast<descriptor::Heap*>(heap);
    const auto step = descriptor::increment(descriptor::kViewsKind);
    CHECK(step.has_value());
    if (!step) {
        release(heap);
        release(buffer);
        return;
    }
    write_raw_srv(buffer, title->start, 4);
    write_raw_srv(buffer, title->start + *step, 8);
    write_raw_srv(buffer, title->start + 2 * *step, 2);
    descriptor::copy_simple(nullptr, 3, title->start + 4 * *step, title->start, descriptor::kViewsKind);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    descriptor_host::sweep_before_submission({});
    if (host_heap != nullptr) {
        for (std::uint32_t index = 0; index < 3; ++index) {
            const std::uint64_t handle = title->start + static_cast<std::uint64_t>(4 + index) * *step;
            const std::uint64_t from = title->start + static_cast<std::uint64_t>(index) * *step;
            const auto view = descriptor_host::materialize_for_draw(heap, 4 + index, handle);
            CHECK(view.has_value());
            CHECK(host_heap->kept(4 + index).Get() != nullptr);
            std::array<std::uint32_t, 3> copied{};
            std::array<std::uint32_t, 3> source{};
            std::memcpy(copied.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(handle)),
                        sizeof copied);
            std::memcpy(source.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(from)),
                        sizeof source);
            CHECK(copied == source);
        }
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap);
    release(buffer);
}

TEST(d3d12_xs_lazy_concurrent_slot_writes_keep_every_bit) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    constexpr std::uint32_t kThreads = 4;
    constexpr std::uint32_t kEach = 16;
    Object* heap = make_heap(kThreads * kEach);
    Object* buffer = make_buffer(256);
    CHECK(heap != nullptr && buffer != nullptr);
    if (heap == nullptr || buffer == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    const std::uint64_t start = static_cast<descriptor::Heap*>(heap)->start;
    const auto step = descriptor::increment(descriptor::kViewsKind);
    CHECK(step.has_value());
    if (!step) {
        release(heap);
        release(buffer);
        return;
    }
    descriptor_host::take_descriptor_stats();
    std::vector<std::thread> threads;
    for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([start, step, buffer, thread] {
            for (std::uint32_t slot = 0; slot < kEach; ++slot) {
                const std::uint32_t index = thread * kEach + slot;
                write_raw_srv(buffer, start + static_cast<std::uint64_t>(index) * *step, 1);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    const descriptor_host::DescriptorStats marked = descriptor_host::take_descriptor_stats();
    CHECK(marked.marked == kThreads * kEach);
    descriptor_host::sweep_before_submission({});
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    if (host_heap != nullptr) {
        for (std::uint32_t index = 0; index < kThreads * kEach; ++index) {
            const std::uint64_t handle = start + static_cast<std::uint64_t>(index) * *step;
            CHECK(descriptor_host::materialize_for_draw(heap, index, handle).has_value());
            CHECK(host_heap->kept(index).Get() != nullptr);
        }
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap);
    release(buffer);
}

TEST(d3d12_xs_lazy_off_mirrors_a_write_before_the_next_draw) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    LazyOff off;
    CHECK(!descriptor_host::lazy_descriptors());
    Object* heap_object = make_heap(3);
    CHECK(heap_object != nullptr);
    if (heap_object == nullptr) return;
    const auto& heap = *static_cast<descriptor::Heap*>(heap_object);
    const auto step = descriptor::increment(descriptor::kViewsKind);
    CHECK(step.has_value());
    if (!step) {
        release(heap_object);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 2;
    srv.Texture2D.MipLevels = 1;
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    words[10] = 0x1000;
    resource::ResourceDesc texture{};
    texture.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.width = 32;
    texture.height = 64;
    texture.depth_or_array_size = 1;
    texture.mip_levels = 4;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.sample_count = 1;
    std::array<std::uint32_t, 14> resource_words{};
    std::memcpy(resource_words.data(), &texture, sizeof texture);
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start);
    CHECK(descriptor_host::for_heap(heap_object) != nullptr);
    const auto binding = descriptor_host::image_heap_data(heap_object);
    CHECK(binding.has_value());
    if (!binding) {
        release(heap_object);
        return;
    }
    using Data = g6shim::shader::emit::ImageHeapData;
    const D3D12_RANGE read_range{0, 3 * Data::kBytes};
    void* mapped = nullptr;
    CHECK(binding->resource->Map(0, &read_range, &mapped) == S_OK);
    if (mapped != nullptr) {
        descriptor::copy_simple(nullptr, 1, heap.start + *step, heap.start, descriptor::kViewsKind);
        Data::Record copied{};
        std::memcpy(copied.data(), static_cast<const std::uint8_t*>(mapped) + Data::kBytes, Data::kBytes);
        constexpr Data::Record kShape{0xC000'0000u, 0x000F'C007u, 0x2000u, 1};
        CHECK(copied == kShape);
        const D3D12_RANGE no_write{0, 0};
        binding->resource->Unmap(0, &no_write);
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    release(heap_object);
}

TEST(d3d12_xs_an_off_element_buffer_view_keeps_its_own_copy) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    LazyOff eager;
    Object* heap = make_heap(4);
    Object* buffer = make_buffer(256);
    CHECK(heap != nullptr && buffer != nullptr);
    if (heap == nullptr || buffer == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(title_words(buffer));
    CHECK(bytes != nullptr);
    if (bytes == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    bytes[8] = 0x5A;
    publish(buffer);
    const std::uint64_t handle = static_cast<descriptor::Heap*>(heap)->start;
    std::array<std::uint32_t, 14> resource{};
    resource[0] = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource[4] = 56;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1;
    srv.Buffer.StructureByteStride = 56;
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(bytes) + 8;
    words[10] = static_cast<std::uint32_t>(address);
    words[11] = static_cast<std::uint32_t>(address >> 32);
    descriptor::create_placed_srv(nullptr, resource.data(), words.data(), handle);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        release(heap);
        release(buffer);
        return;
    }
    const auto view = descriptor_host::materialize_for_draw(heap, 0, handle);
    CHECK(view.has_value());
    CHECK(host_heap->kept(0).Get() != nullptr);
    release(heap);
    release(buffer);
}
