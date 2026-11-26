// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/host.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>



namespace {

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

constexpr std::uint16_t kFixtureVgprs = 8;
constexpr std::uint8_t kConstantsUserData = 0;
constexpr std::uint8_t kTableUserData = 4;
constexpr std::uint8_t kConstantsSlot = 0;
constexpr std::uint8_t kTableSlot = 1;
constexpr std::uint32_t kUserSgprs = 5;
constexpr std::uint32_t kUserSgprShift = 1;
constexpr std::uint32_t kSecondDescriptorDword = 8;
constexpr std::string_view kScalarBufferLength = "scalar_buffer_length";
constexpr std::string_view kScalarBufferCountOnly = "scalar_buffer_count_only";
constexpr std::string_view kScalarBufferAddress = "scalar_buffer_address";
constexpr std::string_view kScalarBufferFields = "scalar_buffer_fields";
constexpr std::string_view kScalarConstantBufferWords = "scalar_constant_buffer_words";
constexpr std::string_view kScalarStructuredFields = "scalar_structured_fields";
constexpr std::string_view kScalarRawHighWord = "scalar_raw_buffer_high_word";
constexpr std::string_view kScalarTypedFormat = "scalar_typed_buffer_format";
constexpr std::string_view kScalarImageAddress = "scalar_image_address";
constexpr std::string_view kImageSampleBiasA16 = "image_sample_bias_a16";
constexpr std::string_view kImageShapeOnly = "image_shape_only";
constexpr std::string_view kImageHeapShape = "image_heap_shape";
constexpr std::string_view kImageHeapShapeLoop = "image_heap_shape_loop";
constexpr std::string_view kBufferHeapStride = "buffer_heap_stride";
constexpr std::string_view kHeapIndexBound = "heap_index_bound";
constexpr std::string_view kRawOffsetBound = "raw_offset_bound";
constexpr std::string_view kHeapTableOffset = "heap_table_offset";
constexpr std::string_view kSamplerLodData = "sampler_lod_data";
constexpr std::string_view kPackedInputPartner = "packed_input_partner";
constexpr std::string_view kSelectReadLaneMask = "select_readlane_mask";
constexpr std::string_view kSpilledComputeRoot = "spilled_compute_root";
constexpr std::string_view kImageGather4Lod = "image_gather4_lod";
constexpr std::string_view kImageGather4LodBorder = "image_gather4_lod_border";
constexpr std::string_view kHeapPointerAsAddress = "heap_pointer_as_address";

const Fixture* find_fixture(std::string_view name) {
    const auto found = std::find_if(std::begin(kFixtures), std::end(kFixtures),
                                   [name](const Fixture& entry) { return entry.name == name; });
    return found == std::end(kFixtures) ? nullptr : found;
}

Lowered lower_fixture(const Fixture& fixture, const std::vector<UsedResource>& used) {
    return lower_compute(fixture.words, kFixtureVgprs,
                         {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
                          root_param(RootType::Kind::Table, kTableUserData, kTableSlot)},
                         used, kUserSgprs << kUserSgprShift, {1, 1, 1});
}

void check_indexed_buffer_results(Lowered lowered) {
    using namespace g6shim::d3d12_xs;
    using Microsoft::WRL::ComPtr;
    host::Host* gpu = host::host();
    auto validator = load_validator();
    if (gpu == nullptr || !validator) return;
    validator->validate(lowered.dxil);

    constexpr std::uint32_t kOutputDescriptor = 0;
    constexpr std::uint32_t kMetadata = 1;
    constexpr std::uint32_t kAlias = 2;
    const auto heap = descriptor_host::HostHeap::create(false, kAlias + 1);
    const auto input = gpu->upload_buffer(64);
    const auto output = gpu->shared_buffer(16);
    using Data = BufferHeapData;
    const std::array<Data::Record, 2> records{{{kAlias + 1, 12, 12, 4}, {kAlias + 1, 20, 8, 2}}};
    const auto metadata = gpu->upload_buffer(sizeof records);
    CHECK((heap && input && output && metadata));
    if (!heap || !input || !output || !metadata) return;
    std::array<std::uint32_t, 16> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = 100 + static_cast<std::uint32_t>(i);
    std::memcpy(input->second, samples.data(), sizeof samples);
    std::memcpy(metadata->second, records.data(), sizeof records);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srv.Buffer.NumElements = static_cast<UINT>(samples.size());
    gpu->device()->CreateShaderResourceView(input->first.Get(), &srv, heap->cpu(kAlias));
    srv.Buffer.NumElements = static_cast<UINT>(records.size()) * Data::kWords;
    gpu->device()->CreateShaderResourceView(metadata->first.Get(), &srv, heap->cpu(kMetadata));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(output->first.Get(), nullptr, &uav, heap->cpu(kOutputDescriptor));

    std::array<std::uint32_t, 64> constants{};
    std::uint32_t count = 0;
    for (const UserData& field : lowered.user_data) {
        if (field.index >= constants.size()) { CHECK(false); return; }
        count = std::max(count, field.index + 1);
        if (field.source == UserData::Source::BufferHeapData) constants[field.index] = kMetadata;
        if (field.source == UserData::Source::ViewHeapCount)
            constants[field.index] = heap->count() + heap->dynamic_slots();
        if (field.source == UserData::Source::NullViewBase) constants[field.index] = heap->count();
        if (field.root.kind == RootType::Kind::Table) constants[field.index] = kOutputDescriptor * 32;
    }
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = count;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)));
    if (!blob) return;
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(gpu->device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&root))));
    if (!root) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {lowered.dxil.data(), lowered.dxil.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
    if (!pipeline) return;
    const auto run = [&](std::uint32_t descriptor, std::uint32_t index, std::uint32_t offset) {
        std::array<std::uint32_t, 3> result{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        const auto list = gpu->new_list();
        if (!list) return result;
        std::memcpy(output->second, result.data(), sizeof result);
        constants[0] = descriptor;
        constants[1] = index;
        constants[2] = offset;
        auto* commands = list->list();
        ID3D12DescriptorHeap* heaps[] = {heap->heap()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetComputeRoot32BitConstants(0, count, constants.data(), 0);
        commands->Dispatch(1, 1, 1);
        list->close();
        gpu->execute({list.get()});
        host::wait_idle();
        std::memcpy(result.data(), output->second, sizeof result);
        return result;
    };
    CHECK((run(0, 1, 0) == std::array{107u, 108u, 109u}));
    CHECK((run(1, 1, 0) == std::array{108u, 109u, 110u}));
    CHECK((run(0, 3, 0) == std::array{0u, 0u, 0u}));
    CHECK((run(0, 0, UINT32_MAX - 3) == std::array{0u, 0u, 0u}));
    CHECK((run(2, 0, 0) == std::array{0u, 0u, 0u}));
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

bool gather_on_warp(const char* label, const std::vector<std::uint8_t>& dxil,
                    const std::vector<std::array<std::uint32_t, 4>>& sampler_words,
                    const std::vector<UserData>& user_data, float u, float v, float lod,
                    std::array<std::uint32_t, 4>* out) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
    for (std::size_t n = 0; n < sampler_words.size(); ++n) {
        const StaticSampler source = StaticSampler::from_words(sampler_words[n]);
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = static_cast<D3D12_FILTER>(source.filter);
        sampler.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[0]);
        sampler.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[1]);
        sampler.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[2]);
        sampler.MipLODBias = source.mip_lod_bias;
        sampler.MaxAnisotropy = source.max_anisotropy;
        sampler.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(source.comparison);
        sampler.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(source.border);
        sampler.MinLOD = source.min_lod;
        sampler.MaxLOD = source.max_lod;
        sampler.ShaderRegister = static_cast<UINT>(n);
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        static_samplers.push_back(sampler);
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
    root_desc.pStaticSamplers = static_samplers.empty() ? nullptr : static_samplers.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                      D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> serialized;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
        return false;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.CS = {dxil.data(), dxil.size()};
    ComPtr<ID3D12PipelineState> state;
    const HRESULT pipeline_hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&state));
    if (FAILED(pipeline_hr)) {
        std::printf("%s pipeline refused %08x\n", label, static_cast<unsigned>(pipeline_hr));
        return false;
    }

    constexpr UINT kMips = 3;
    constexpr UINT kWidth = 4;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = kWidth;
    texture_desc.Height = kWidth;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = kMips;
    texture_desc.Format = DXGI_FORMAT_R32_FLOAT;
    texture_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kMips]{};
    UINT64 upload_bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, kMips, 0, footprints, nullptr, nullptr, &upload_bytes);
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return false;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    auto* bytes = static_cast<std::uint8_t*>(mapped);
    std::memset(bytes, 0, static_cast<std::size_t>(upload_bytes));
    const auto put = [&](UINT mip, UINT x, UINT y, float value) {
        const std::uint32_t bits = float_bits(value);
        std::memcpy(bytes + footprints[mip].Offset + y * footprints[mip].Footprint.RowPitch + x * sizeof bits, &bits,
                    sizeof bits);
    };
    for (UINT y = 0; y < kWidth; ++y)
        for (UINT x = 0; x < kWidth; ++x) put(0, x, y, 10.0f);
    put(1, 0, 0, 1.0f);
    put(1, 1, 0, 2.0f);
    put(1, 0, 1, 3.0f);
    put(1, 1, 1, 4.0f);
    put(2, 0, 0, 20.0f);
    upload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC output_desc = upload_desc;
    output_desc.Width = 16;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))))
        return false;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    output_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> resources;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&resources)))) return false;
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = resources->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = kMips;
    device->CreateShaderResourceView(texture.Get(), &srv, cpu);
    cpu.ptr += step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpu);

    D3D12_RESOURCE_DESC constants_desc = upload_desc;
    constants_desc.Width = 256;
    ComPtr<ID3D12Resource> constants;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &constants_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants))))
        return false;
    if (FAILED(constants->Map(0, nullptr, &mapped))) return false;
    auto* words = static_cast<std::uint32_t*>(mapped);
    std::fill(words, words + 64, 0u);
    words[0] = float_bits(u);
    words[1] = float_bits(v);
    words[2] = float_bits(lod);
    for (const UserData& field : user_data) {
        if (field.index >= 64) return false;
        if (field.source == UserData::Source::ViewHeapCount ||
            field.source == UserData::Source::SamplerHeapCount)
            words[field.index] = 2;
    }
    constants->Unmap(0, nullptr);

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        return false;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return false;
    for (UINT mip = 0; mip < kMips; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[mip];
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER to_sample{};
    to_sample.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_sample.Transition.pResource = texture.Get();
    to_sample.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_sample.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    to_sample.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_BARRIER to_uav = to_sample;
    to_uav.Transition.pResource = output.Get();
    to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_BARRIER ready[] = {to_sample, to_uav};
    list->ResourceBarrier(2, ready);
    ID3D12DescriptorHeap* heaps[] = {resources.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(state.Get());
    list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy = to_uav;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(readback.Get(), output.Get());
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
    if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
    std::memcpy(out->data(), mapped, sizeof *out);
    readback->Unmap(0, nullptr);
    return true;
}


}  // namespace

TEST(shader_fixtures_translate_and_validate) {
    CHECK(std::size(kFixtures) > 0);
    for (const Fixture& fixture : kFixtures) {
        if (fixture.name == kScalarBufferLength || fixture.name == kScalarBufferAddress ||
            fixture.name == kScalarBufferFields || fixture.name == kScalarStructuredFields ||
            fixture.name == kScalarRawHighWord || fixture.name == kScalarTypedFormat ||
            fixture.name == kScalarImageAddress || fixture.name == kScalarConstantBufferWords ||
            fixture.name == kImageSampleBiasA16 || fixture.name == kScalarBufferCountOnly ||
            fixture.name == kPackedInputPartner || fixture.name == kSelectReadLaneMask ||
            fixture.name == kImageShapeOnly || fixture.name == kImageHeapShape ||
            fixture.name == kImageHeapShapeLoop || fixture.name == kHeapIndexBound ||
            fixture.name == kHeapTableOffset || fixture.name == kBufferHeapStride ||
            fixture.name == kRawOffsetBound ||
            fixture.name == kSamplerLodData || fixture.name == kSpilledComputeRoot ||
            fixture.name == kImageGather4Lod || fixture.name == kImageGather4LodBorder ||
            fixture.name == kHeapPointerAsAddress)
            continue;
        bool translated = true;
        Lowered lowered;
        try {
            lowered = lower_fixture(fixture, {used_uav(kTableUserData, kTableSlot)});
        } catch (const std::exception& error) {
            std::printf("fixture %s did not translate: %s\n", fixture.name, error.what());
            translated = false;
        }
        CHECK(translated);
        if (!translated) continue;
        std::size_t heap_limits = 0;
        for (const UserData& entry : lowered.user_data) {
            if (entry.source == UserData::Source::ViewHeapCount ||
                entry.source == UserData::Source::SamplerHeapCount ||
                entry.source == UserData::Source::NullViewBase)
                ++heap_limits;
        }
        CHECK(lowered.user_data.size() == kUserSgprs + heap_limits);
        check_dxil(lowered.dxil, fixture.name);
    }
}

TEST(shader_fixtures_translate_spilled_compute_root_words) {
    const Fixture* fixture = find_fixture(kSpilledComputeRoot);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kSpilledTable = 16;
    constexpr std::uint8_t kSpilledConstants = 18;
    constexpr std::uint32_t kSpillPointerUserSgprs = 4;
    RootParam table = root_param(RootType::Kind::Table, kSpilledTable, kTableSlot);
    RootParam constants = root_param(RootType::Kind::Constants, kSpilledConstants, kConstantsSlot);
    table.sgprs = constants.sgprs = 2;
    try {
        const Lowered lowered = lower_compute(fixture->words, kFixtureVgprs, {table, constants},
                                              {used_uav(kSpilledTable, kTableSlot)},
                                              kSpillPointerUserSgprs << kUserSgprShift, {1, 1, 1});
        const auto has_root = [&](std::uint32_t index, std::uint8_t slot) {
            return std::any_of(lowered.user_data.begin(), lowered.user_data.end(), [&](const UserData& word) {
                return word.source == UserData::Source::Root && word.index == index && word.slot == slot &&
                       word.dword == 0;
            });
        };
        CHECK(has_root(kSpilledTable, kTableSlot));
        CHECK(has_root(kSpilledConstants, kConstantsSlot));
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
    bool rejected = false;
    try {
        (void)lower_compute(fixture->words, kFixtureVgprs, {table, constants},
                            {used_uav(kSpilledTable, kTableSlot)},
                            (kSpillPointerUserSgprs + 1) << kUserSgprShift, {1, 1, 1});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("an address in an SGPR that is no root parameter") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_translate_biased_sample_with_packed_coordinates) {
    const Fixture* fixture = find_fixture(kImageSampleBiasA16);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;

    UsedResource texture = used_uav(kTableUserData, kTableSlot);
    texture.kind.kind = ResourceType::Kind::Srv;
    texture.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    constexpr std::uint8_t kSamplerUserData = 6;
    constexpr std::uint8_t kSamplerSlot = 2;
    NativeProgram native;
    native.vgprs = kFixtureVgprs;
    native.layout = {root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
                     root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot)};
    native.used = {texture, output};
    native.code = bytes_of(fixture->words);
    for (const HwStage stage : {HwStage::Ps, HwStage::Cs}) {
        native.stage = stage;
        if (stage == HwStage::Ps)
            native.io = PsIo{};
        else
            native.io = ComputeIo{(kSamplerUserData + 1u) << kUserSgprShift, {32, 1, 1}};
        try {
            const Lowered lowered = lower_native(native);
            check_dxil(lowered.dxil, stage == HwStage::Ps ? fixture->name : "image_sample_compute");
        } catch (const std::exception& error) {
            std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
            CHECK(false);
        }
    }
}

TEST(shader_fixtures_translate_an_implicit_packed_pixel_input_pair) {
    const Fixture* fixture = find_fixture(kPackedInputPartner);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;

    NativeProgram native;
    native.stage = HwStage::Ps;
    native.vgprs = kFixtureVgprs;
    PsIo io;
    io.input_addr = 2;
    PsInput packed;
    packed.usage = Usage{Usage::Kind::Generic, 0};
    packed.mask = 15;
    packed.fp16 = true;
    packed.pack2 = true;
    io.inputs.push_back(packed);
    native.io = io;
    native.code = bytes_of(fixture->words);

    const std::vector<SigElement> inputs{
        {"TEXCOORD", 0, 0, 9, 2, 0u, 0, 4, 0, true},
        {"TEXCOORD", 1, 0, 9, 2, 1u, 0, 4, 0, true},
    };
    const std::vector<SigElement> outputs{{"SV_Target", 0, 16, 9, 0, 0u, 0, 4, 0, false}};
    try {
        check_dxil(lower_native(native, inputs, outputs).dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    PsInput independent;
    independent.usage = Usage{Usage::Kind::Generic, 1};
    independent.row = 1;
    independent.row2 = 1;
    independent.mask = 15;
    independent.attr = 1;
    io.inputs.push_back(independent);
    native.io = io;
    bool rejected = false;
    try {
        (void)lower_native(native, inputs, outputs);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a packed input half no element holds") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    auto separated_inputs = inputs;
    separated_inputs[1].row = 3;
    separated_inputs[1].cols = 2;
    SigElement separate = inputs[1];
    separate.index = 2;
    separate.half = false;
    separated_inputs.push_back(separate);
    try {
        check_dxil(lower_native(native, separated_inputs, outputs).dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate a separated partner: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_a_select_on_raw_lane_bits) {
    const Fixture* fixture = find_fixture(kSelectReadLaneMask);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.vgprs = kFixtureVgprs;
    native.io = ComputeIo{kUserSgprs << kUserSgprShift, {32, 2, 1}};
    native.layout = {root_param(RootType::Kind::Table, kTableUserData, kTableSlot)};
    native.used = {used_uav(kTableUserData, kTableSlot)};
    native.code = bytes_of(fixture->words);
    for (const bool wave64 : {false, true}) {
        native.wave64 = wave64;
        try {
            check_dxil(lower_native(native).dxil, fixture->name);
        } catch (const std::exception& error) {
            std::printf("fixture %s (wave %d) did not translate: %s\n", fixture->name, wave64 ? 64 : 32,
                        error.what());
            CHECK(false);
        }
    }
}

TEST(shader_fixtures_translate_a_wave64_lane_mask_join) {
    const std::vector<std::uint32_t> words = {
        0xBE850380u, 0xF4080202u, 0xFA000000u, 0x7E020200u, 0x7E040201u, 0x7E060202u, 0x7E080203u,
        0xD7650005u, 0x020100C1u, 0xBF068000u, 0xBF850003u, 0xD4C10006u, 0x02000305u, 0xBF820001u,
        0xBE860480u, 0xD5010000u, 0x00190280u, 0x340A0A85u, 0xBF8CC07Fu, 0xE0701000u, 0x80020005u,
        0xBF810000u};
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.wave64 = true;
    native.vgprs = kFixtureVgprs;
    native.io = ComputeIo{kUserSgprs << kUserSgprShift, {1, 1, 1}};
    native.layout = {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
                     root_param(RootType::Kind::Table, kTableUserData, kTableSlot)};
    native.used = {used_uav(kTableUserData, kTableSlot)};
    native.code = bytes_of(words);
    bool translated = true;
    try {
        check_dxil(lower_native(native).dxil, "lane_mask_join_wave64");
    } catch (const std::exception& error) {
        std::printf("wave64 lane-mask join did not translate: %s\n", error.what());
        translated = false;
    }
    CHECK(translated);
}

TEST(shader_fixtures_translate_image_dimensions_without_an_image_operation) {
    const Fixture* fixture = find_fixture(kImageShapeOnly);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const auto lowered = lower_fixture(*fixture, {input, output});
        using Source = UserData::Source;
        for (const auto source : {Source::ImageWidthLowBits, Source::ImageDimensions, Source::ImageBaseMip}) {
            const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                [source](const UserData& entry) { return entry.source == source; });
            CHECK(field != lowered.user_data.end());
            if (field != lowered.user_data.end()) CHECK(field->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    Fixture unknown = *fixture;
    const auto shift = std::find(unknown.words.begin(), unknown.words.end(), 0x9006'9E09u);
    CHECK(shift != unknown.words.end());
    if (shift == unknown.words.end()) return;
    *shift = 0x9006'9D09u;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture unknown_mip = *fixture;
    const auto extract = std::find(unknown_mip.words.begin(), unknown_mip.words.end(), 0x0004'000Cu);
    CHECK(extract != unknown_mip.words.end());
    if (extract == unknown_mip.words.end()) return;
    ++*extract;
    rejected = false;
    try {
        (void)lower_fixture(unknown_mip, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture variable = *fixture;
    const auto alignment = std::find(variable.words.begin(), variable.words.end(), 0x0278'120Au);
    CHECK(alignment != variable.words.end());
    if (alignment == variable.words.end()) return;
    *alignment &= ~(0x1FFu << 18);
    rejected = false;
    try {
        (void)lower_fixture(variable, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_translate_sampler_lod_data) {
    const Fixture* fixture = find_fixture(kSamplerLodData);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kSamplerUserData = 6;
    constexpr std::uint8_t kSamplerSlot = 2;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
             root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot)},
            {used_uav(kTableUserData, kTableSlot)}, (kSamplerUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::SamplerLodWord;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::SamplerHeap);
            CHECK(field->slot == kSamplerSlot);
            CHECK(field->dword == 4);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_image_shape_at_a_computed_heap_index) {
    const Fixture* fixture = find_fixture(kImageHeapShape);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    const auto translate = [&](const Fixture& code) {
        return lower_compute(code.words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
    };
    try {
        const auto lowered = translate(*fixture);
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ImageHeapData;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->slot == kHeapSlot);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    Fixture unknown = *fixture;
    const auto align = std::find(unknown.words.begin(), unknown.words.end(), 0x0278'120Eu);
    CHECK(align != unknown.words.end());
    if (align == unknown.words.end()) return;
    *align += 1u << 18;
    bool rejected = false;
    try {
        (void)translate(unknown);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture unused = *fixture;
    unused.words.erase(unused.words.begin() + 4, unused.words.end() - 1);
    try {
        const auto lowered = translate(unused);
        CHECK(std::none_of(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ImageHeapData;
        }));
        check_dxil(lowered.dxil, unused.name);
    } catch (const std::exception& error) {
        std::printf("fixture %s with an unused load did not translate: %s\n", unused.name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_heap_shape_read_in_a_loop_creates_a_pipeline_on_the_default_adapter) {
    const Fixture* fixture = find_fixture(kImageHeapShapeLoop);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    constexpr std::uint32_t kHostUserDataDwords = 16;
    std::vector<std::uint8_t> dxil;
    std::uint32_t constants = kHostUserDataDwords;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        check_dxil(lowered.dxil, fixture->name);
        dxil = lowered.dxil;
        for (const UserData& field : lowered.user_data) constants = std::max(constants, field.index + 1);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::printf("skipped: no D3D12 device\n");
        return;
    }
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = constants;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                      D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)));
    if (!blob) return;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                IID_PPV_ARGS(&root))));
    if (!root) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {dxil.data(), dxil.size()};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
}

TEST(shader_fixtures_translate_a_buffer_load_with_a_runtime_stride) {
    const Fixture* fixture = find_fixture(kBufferHeapStride);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::BufferHeapData;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->slot == kHeapSlot);
        }
        check_dxil(lowered.dxil, fixture->name);
        check_indexed_buffer_results(lowered);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_bound_a_descriptor_heap_index) {
    const Fixture* fixture = find_fixture(kHeapIndexBound);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ViewHeapCount;
        });
        CHECK(field != lowered.user_data.end());
        CHECK(std::any_of(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::NullViewBase &&
                   entry.root.kind == RootType::Kind::ResourceHeap;
        }));
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->index >= kOutputUserData + 1);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}
namespace {

bool bitcode_names_contain(const std::vector<std::uint8_t>& dxil, std::string_view text) {
    std::size_t at = 0;
    bool found_magic = false;
    for (; at + 4 <= dxil.size(); ++at) {
        if (dxil[at] == 'B' && dxil[at + 1] == 'C' && dxil[at + 2] == 0xC0 && dxil[at + 3] == 0xDE) {
            found_magic = true;
            break;
        }
    }
    if (!found_magic) return false;
    at += 4;
    std::vector<std::uint32_t> words;
    for (std::size_t i = at; i + 4 <= dxil.size(); i += 4) {
        words.push_back(static_cast<std::uint32_t>(dxil[i]) | static_cast<std::uint32_t>(dxil[i + 1]) << 8 |
                        static_cast<std::uint32_t>(dxil[i + 2]) << 16 |
                        static_cast<std::uint32_t>(dxil[i + 3]) << 24);
    }
    struct Bits {
        const std::vector<std::uint32_t>* words;
        std::size_t index = 0;
        std::uint64_t current = 0;
        int have = 0;
        std::uint32_t take(int width) {
            while (have < width && index < words->size()) {
                current |= static_cast<std::uint64_t>((*words)[index++]) << have;
                have += 32;
            }
            const std::uint32_t value = static_cast<std::uint32_t>(current & ((std::uint64_t{1} << width) - 1));
            current >>= width;
            have -= width;
            return value;
        }
        std::uint64_t vbr(int width) {
            const std::uint64_t high = std::uint64_t{1} << (width - 1);
            std::uint64_t value = 0;
            int shift = 0;
            for (;;) {
                const std::uint32_t chunk = take(width);
                value |= static_cast<std::uint64_t>(chunk & (high - 1)) << shift;
                if ((chunk & high) == 0) return value;
                shift += width - 1;
            }
        }
        void align() {
            current = 0;
            have = 0;
        }
        std::uint32_t word() {
            align();
            return index < words->size() ? (*words)[index++] : 0;
        }
    };
    Bits bits{&words};
    bool hit = false;
    const auto search = [&](auto&& self, int abbrev) -> void {
        if (hit) return;
        for (;;) {
            if (bits.index >= words.size() && bits.have < abbrev) return;
            const std::uint32_t id = bits.take(abbrev);
            if (id == 0) {
                bits.align();
                return;
            }
            if (id == 1) {
                (void)bits.vbr(8);
                const std::uint64_t next = bits.vbr(4);
                bits.align();
                (void)bits.word();
                self(self, static_cast<int>(next == 0 ? 2 : next));
                if (hit) return;
                continue;
            }
            if (id != 3) return;
            (void)bits.vbr(6);
            const std::uint64_t count = bits.vbr(6);
            std::string run;
            for (std::uint64_t n = 0; n < count; ++n) {
                const std::uint64_t op = bits.vbr(6);
                if (op >= 32 && op < 127)
                    run.push_back(static_cast<char>(op));
                else
                    run.clear();
                if (run.find(text) != std::string::npos) {
                    hit = true;
                    return;
                }
            }
        }
    };
    search(search, 2);
    return hit;
}

}  // namespace


TEST(shader_fixtures_bound_a_raw_buffer_byte_offset) {
    const Fixture* fixture = find_fixture(kRawOffsetBound);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        CHECK(bitcode_names_contain(lowered.dxil, "getDimensions"));
        CHECK(bitcode_names_contain(lowered.dxil, "rawBufferLoad"));
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_bound_a_constant_table_offset) {
    const Fixture* fixture = find_fixture(kHeapTableOffset);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ViewHeapCount;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->index >= kOutputUserData + 1);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_a_buffer_count_without_using_the_view) {
    const Fixture* fixture = find_fixture(kScalarBufferCountOnly);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        const auto count = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ShaderBufferRecordCount;
        });
        CHECK(count != lowered.user_data.end());
        if (count != lowered.user_data.end()) {
            CHECK(count->slot == kTableSlot);
            CHECK(count->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_scalar_buffer_length_and_load) {
    const Fixture* fixture = find_fixture(kScalarBufferLength);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    for (const std::uint32_t stride : {0u, 1u}) {
        input.stride = stride;
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        check_dxil(lowered.dxil, fixture->name);
    }
}

TEST(shader_fixtures_translate_scalar_buffer_address_at_a_join) {
    const Fixture* fixture = find_fixture(kScalarBufferAddress);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    using Kind = ResourceType::Kind;
    using Source = UserData::Source;
    for (const Kind kind : {Kind::Srv, Kind::Cbv}) {
        input.kind.kind = kind;
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        const Source source = kind == Kind::Cbv ? Source::ConstantBufferAddressLow : Source::ShaderBufferAddressLow;
        const auto address = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                          [source](const UserData& entry) { return entry.source == source; });
        CHECK(address != lowered.user_data.end());
        if (address != lowered.user_data.end()) {
            CHECK(address->index >= kUserSgprs);
            CHECK(address->slot == kTableSlot);
            CHECK(address->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    }
}

TEST(shader_fixtures_translate_scalar_constant_buffer_words) {
    const Fixture* fixture = find_fixture(kScalarConstantBufferWords);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Cbv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        using Source = UserData::Source;
        for (const Source source : {Source::ConstantBufferHighWord, Source::ConstantBufferRecordCount,
                                    Source::ConstantBufferFormatWord}) {
            const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                            [source](const UserData& entry) { return entry.source == source; });
            CHECK(field != lowered.user_data.end());
            if (field != lowered.user_data.end()) {
                CHECK(field->slot == kTableSlot);
                CHECK(field->dword == 0);
            }
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_masked_scalar_buffer_fields_at_a_join) {
    const Fixture* fixture = find_fixture(kScalarBufferFields);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 2;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::ShaderBufferAddressHigh, Source::ShaderBufferComponentMapping}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& entry) { return entry.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);

    Fixture unknown = *fixture;
    const auto mask = std::find(unknown.words.begin(), unknown.words.end(), UserData::kBufferAddressHighMask);
    CHECK(mask != unknown.words.end());
    if (mask == unknown.words.end()) return;
    *mask |= UserData::kBufferAddressHighMask + 1;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_translate_structured_buffer_stride_and_zero_test) {
    const Fixture* fixture = find_fixture(kScalarStructuredFields);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 16;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::ShaderBufferAddressLow, Source::StructuredBufferHighWord,
                                Source::StructuredBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_raw_buffer_high_word_with_vector_user) {
    const Fixture* fixture = find_fixture(kScalarRawHighWord);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 1;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::LinearRawBufferHighWord, Source::LinearRawBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_the_typed_buffer_format_word) {
    const Fixture* fixture = find_fixture(kScalarTypedFormat);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 4;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::TypedBufferHighWord, Source::TypedBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_scalar_reads_of_image_addresses) {
    const Fixture* fixture = find_fixture(kScalarImageAddress);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    const auto address = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
        return entry.source == UserData::Source::ImageBaseAddressLow;
    });
    CHECK(address != lowered.user_data.end());
    if (address != lowered.user_data.end()) {
        CHECK(address->index >= kUserSgprs);
        CHECK(address->slot == kTableSlot);
        CHECK(address->dword == 0);
    }
    const auto sampling = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
        return entry.source == UserData::Source::ImageSamplingFields;
    });
    CHECK(sampling != lowered.user_data.end());
    if (sampling != lowered.user_data.end()) {
        CHECK(sampling->slot == kTableSlot);
        CHECK(sampling->dword == 0);
    }
    check_dxil(lowered.dxil, fixture->name);

    Fixture unknown = *fixture;
    constexpr std::uint32_t kPerformanceBit = 1u << UserData::kImageSamplingPerformanceShift;
    constexpr std::uint32_t kCornerSampling = 1u << 23;
    const auto mask = std::find(unknown.words.begin(), unknown.words.end(), kPerformanceBit);
    CHECK(mask != unknown.words.end());
    if (mask == unknown.words.end()) return;
    *mask = kCornerSampling;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_gather_four_texels_at_a_level) {
    const Fixture* lod = find_fixture(kImageGather4Lod);
    const Fixture* border = find_fixture(kImageGather4LodBorder);
    CHECK(lod != nullptr && border != nullptr);
    if (lod == nullptr || border == nullptr) return;

    constexpr std::uint8_t kSamplerUserData = 6;
    constexpr std::uint8_t kSamplerSlot = 2;
    UsedResource texture = used_uav(kTableUserData, kTableSlot);
    texture.kind.kind = ResourceType::Kind::Srv;
    texture.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const auto lower_gather = [&](const Fixture& fixture, bool sampler) {
        std::vector<RootParam> layout = {
            root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
            root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
        };
        std::uint32_t users = kUserSgprs;
        if (sampler) {
            layout.push_back(root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot));
            users = kSamplerUserData + 1;
        }
        return lower_compute(fixture.words, kFixtureVgprs, layout, {texture, output}, users << kUserSgprShift,
                             {1, 1, 1});
    };
    bool translated = true;
    Lowered heap;
    Lowered edged;
    try {
        heap = lower_gather(*lod, true);
        edged = lower_gather(*border, false);
    } catch (const std::exception& error) {
        std::printf("gather fixture did not translate: %s\n", error.what());
        translated = false;
    }
    CHECK(translated);
    if (!translated) return;
    check_dxil(heap.dxil, lod->name);
    check_dxil(edged.dxil, border->name);


    std::array<std::uint32_t, 4> center{};
    std::array<std::uint32_t, 4> edge{};
    const bool center_ran = gather_on_warp("heap", heap.dxil, heap.samplers, heap.user_data, 0.5f, 0.5f, 1.0f, &center);
    const bool edge_ran = gather_on_warp("border", edged.dxil, edged.samplers, edged.user_data, 0.0f, 0.0f, 1.0f, &edge);
    if (!center_ran || !edge_ran) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    const std::array<std::uint32_t, 4> center_expected{float_bits(3.0f), float_bits(4.0f), float_bits(2.0f),
                                                      float_bits(1.0f)};
    const std::array<std::uint32_t, 4> edge_expected{0u, float_bits(1.0f), 0u, 0u};
    CHECK(center == center_expected);
    CHECK(edge == edge_expected);
}

TEST(shader_fixtures_refuse_a_heap_pointer_used_as_an_address) {
    const Fixture* fixture = find_fixture(kHeapPointerAsAddress);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 1;
    bool refused = false;
    try {
        lower_compute(fixture->words, kFixtureVgprs,
                      {root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot)}, {},
                      (kHeapUserData + 1u) << kUserSgprShift, {1, 1, 1});
    } catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("a descriptor heap pointer used as an address") !=
                  std::string_view::npos;
    }
    CHECK(refused);
}
