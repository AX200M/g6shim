// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/draw.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/list.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/pipeline.hpp"
#include "d3d12_xs/pipeline_bridge.hpp"
#include "d3d12_xs/queue.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/texture.hpp"

#include "g6shim/shader/emit/container.hpp"

#include "g_depth_equal_ps.h"
#include "g_depth_equal_vs.h"
#include "g_depth_uav_write.h"
#include <algorithm>
#include <array>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

struct Environment {
    Environment() {
        ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) == 0) return;
        const std::filesystem::path path = std::filesystem::path(temp) / L"g6shim_depth_uav_test";
        ::SetEnvironmentVariableW(L"G6SHIM_CACHE_DIR", path.c_str());
    }
};

const Environment g_environment;

void append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void append64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append32(out, static_cast<std::uint32_t>(value));
    append32(out, static_cast<std::uint32_t>(value >> 32));
}

void set32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void set64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
    set32(out, at, static_cast<std::uint32_t>(value));
    set32(out, at + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t adler(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

std::vector<std::uint8_t> zlib(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - at);
        const bool final = at + length == data.size();
        out.push_back(final ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
        out.push_back(static_cast<std::uint8_t>(inverse));
        out.push_back(static_cast<std::uint8_t>(inverse >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < data.size());
    const std::uint32_t check = adler(data);
    for (const std::uint32_t shift : {24u, 16u, 8u, 0u}) out.push_back(static_cast<std::uint8_t>(check >> shift));
    return out;
}

std::vector<std::uint8_t> xbch(const std::vector<std::uint8_t>& unpacked) {
    constexpr std::uint32_t kXbchVersion = 3;
    const std::vector<std::uint8_t> stream = zlib(unpacked);
    std::vector<std::uint8_t> out{'X', 'B', 'C', 'H'};
    append32(out, kXbchVersion);
    for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(stream.size() >> (8 * i)));
    for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(unpacked.size() >> (8 * i)));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

std::vector<std::uint8_t> compute_container_using_uav() {
    constexpr std::uint32_t kKindNative = 0x0D;
    constexpr std::uint32_t kFssmVersion = 0x101;
    constexpr std::size_t kXdatOffset = 0x18;
    constexpr std::uint64_t kGraphicsSlotCount = 6;
    constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFULL;
    constexpr std::size_t kRecordSize = 0x54;
    constexpr std::size_t kRecordFlags = 0x14;
    constexpr std::size_t kRecordUsed = 0x30;
    constexpr std::size_t kRecordUsedCount = 0x3C;
    constexpr std::size_t kRecordRegs = 0x50;
    constexpr std::size_t kCsSlot = 0x60;
    constexpr std::size_t kStageBytes = 0x38;
    constexpr std::size_t kCsThreads = 0x28;
    constexpr std::uint32_t kEndpgm = 0xBF81'0000;
    constexpr std::array<std::size_t, 8> kSlots{0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60};
    std::vector<std::uint8_t> native_code;
    append32(native_code, kEndpgm);
    std::vector<std::uint8_t> stage(kStageBytes, 0);
    set32(stage, kCsThreads, 1);
    set32(stage, kCsThreads + 4, 1);
    set32(stage, kCsThreads + 8, 1);
    std::vector<std::uint8_t> payload;
    append64(payload, kGraphicsSlotCount);
    payload.resize(kCsSlot + 8, 0);
    for (const std::size_t at : kSlots) set64(payload, at, kAbsent);
    const std::uint64_t record = payload.size();
    set64(payload, kCsSlot, record);
    append64(payload, record + kRecordSize + stage.size());
    append64(payload, native_code.size());
    payload.resize(static_cast<std::size_t>(record) + kRecordFlags, 0);
    append32(payload, 0);
    payload.resize(static_cast<std::size_t>(record) + kRecordSize, 0);
    set32(payload, static_cast<std::size_t>(record) + kRecordRegs, 28 | 31U << 20);
    payload.insert(payload.end(), stage.begin(), stage.end());
    payload.insert(payload.end(), native_code.begin(), native_code.end());
    const std::uint64_t used_at = payload.size();
    std::array<std::uint8_t, 20> used{};
    used[0] = 3;
    used[1] = 3;
    payload.insert(payload.end(), used.begin(), used.end());
    set64(payload, static_cast<std::size_t>(record) + kRecordUsed, used_at);
    set32(payload, static_cast<std::size_t>(record) + kRecordUsedCount, 1);
    std::vector<std::uint8_t> fssm{'F', 'S', 'S', 'M'};
    append32(fssm, kFssmVersion);
    for (std::size_t i = 0; i < 8; ++i) fssm.push_back(static_cast<std::uint8_t>(payload.size() >> (8 * i)));
    fssm.insert(fssm.end(), payload.begin(), payload.end());
    const std::vector<std::uint8_t> packed = xbch(fssm);
    std::vector<std::uint8_t> xdat(kXdatOffset, 0);
    xdat.insert(xdat.end(), {'X', 'D', 'A', 'T'});
    append32(xdat, 1);
    append32(xdat, 0x24);
    const std::uint32_t native_at = static_cast<std::uint32_t>(xdat.size() + 12);
    append32(xdat, kKindNative);
    append32(xdat, native_at);
    append32(xdat, static_cast<std::uint32_t>(packed.size()));
    xdat.insert(xdat.end(), packed.begin(), packed.end());
    const std::array<g6shim::shader::emit::ContainerPart, 1> parts{
        {g6shim::shader::emit::ContainerPart{{'D', 'X', 'I', 'L'}, xdat}}};
    return g6shim::shader::emit::build(parts);
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> empty_root(ID3D12Device* device) {
    D3D12_ROOT_SIGNATURE_DESC desc{};
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        return nullptr;
    return root;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> uav_root(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        return nullptr;
    return root;
}

Microsoft::WRL::ComPtr<ID3D12Resource> readback(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

void drop_object(Object* object) {
    if (object != nullptr) slot<std::uint32_t (*)(Object*)>(object, 0x10)(object);
}

}  // namespace

TEST(d3d12_xs_depth_unordered_write_matches_depth_test) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    constexpr std::uint32_t kWidth = 4;
    constexpr std::uint32_t kDepthWrite = 0x10;
    constexpr std::uint16_t kFar = 8192;
    constexpr std::uint16_t kHit = 16384;
    resource::ResourceDesc desc{};
    desc.dimension = 3;
    desc.width = kWidth;
    desc.height = 1;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 1;
    desc.format = 55;
    desc.sample_count = 1;
    desc.flags = 2 | 4;
    std::array<std::uint32_t, 5> clear{};
    clear[0] = 55;
    const float one = 1.0f;
    std::memcpy(&clear[1], &one, sizeof one);
    const resource::HeapProperties properties{1, 0, 0, 1, 1};
    Object* depth = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &desc, kDepthWrite, clear.data(),
                                     &resource::kResourceIids[0], &depth) == S_OK);
    CHECK(depth != nullptr);
    if (depth == nullptr) return;
    ID3D12Resource* host_depth = texture::host_texture(depth);
    CHECK(host_depth != nullptr);
    if (host_depth == nullptr) {
        drop_object(depth);
        return;
    }

    const descriptor::HeapDesc heap_desc{0, 1, descriptor::kShaderVisible, 0};
    Object* heap = nullptr;
    CHECK(descriptor::create_heap(nullptr, &heap_desc, &descriptor::kIids[0], &heap) == S_OK);
    if (heap == nullptr) {
        drop_object(depth);
        return;
    }
    const std::uint64_t view = static_cast<descriptor::Heap*>(heap)->start;
    std::array<std::uint32_t, 10> uav{};
    uav[0] = 56;
    uav[1] = 4;
    descriptor::create_uav(nullptr, depth, nullptr, uav.data(), view);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    if (host_heap == nullptr) {
        drop_object(heap);
        drop_object(depth);
        return;
    }
    CHECK(host_heap->mirror(0, view, false).has_value());

    const Microsoft::WRL::ComPtr<ID3D12RootSignature> compute_root = uav_root(gpu->device());
    CHECK(compute_root != nullptr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
    compute_desc.pRootSignature = compute_root.Get();
    compute_desc.CS = {g_depth_uav_write, sizeof g_depth_uav_write};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compute;
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute))));
    if (compute == nullptr) {
        drop_object(heap);
        drop_object(depth);
        return;
    }

    Object* allocator = nullptr;
    CHECK(list::create_allocator(nullptr, 0, &list::kAllocatorIids[0], &allocator) == S_OK);
    Object* commands = nullptr;
    CHECK(list::create_list(nullptr, 1, 0, allocator, nullptr, &list::kListIids[0], &commands) == S_OK);
    host::HostList* recorded = list::host_list(commands);
    CHECK(recorded != nullptr);
    if (recorded == nullptr) {
        drop_object(commands);
        drop_object(allocator);
        drop_object(heap);
        drop_object(depth);
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {host_heap->heap()};
    recorded->list()->SetDescriptorHeaps(1, heaps);
    recorded->list()->SetComputeRootSignature(compute_root.Get());
    recorded->list()->SetPipelineState(compute.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_view{};
    gpu_view.ptr = host_heap->gpu_start();
    recorded->list()->SetComputeRootDescriptorTable(0, gpu_view);
    recorded->list()->Dispatch(1, 1, 1);

    const std::vector<std::uint8_t> container = compute_container_using_uav();
    pipeline::ComputeHead pipeline_desc{};
    pipeline_desc.stage = pipeline::Bytecode{container.data(), container.size()};
    Object* title_pipeline = nullptr;
    CHECK(pipeline::create_compute(&pipeline_desc, &pipeline::kIids[0], &title_pipeline) == S_OK);
    pipeline_host::HostPipeline* host_pipeline = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (host_pipeline == nullptr && std::chrono::steady_clock::now() < deadline) {
        host_pipeline = pipeline_bridge::host_pipeline_for(title_pipeline, true);
        if (host_pipeline == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(host_pipeline != nullptr);
    if (host_pipeline == nullptr) {
        drop_object(title_pipeline);
        drop_object(commands);
        drop_object(allocator);
        drop_object(heap);
        drop_object(depth);
        return;
    }
    const Object* bound_heaps[] = {heap};
    slot<void (*)(Object*, std::uint32_t, const Object* const*)>(commands, 0x100)(commands, 1, bound_heaps);
    slot<void (*)(Object*, std::uint32_t, std::uint64_t)>(commands, 0x118)(commands, 0, view);
    slot<void (*)(Object*, Object*)>(commands, 0xc8)(commands, title_pipeline);
    const std::uint64_t before = draw::dispatched();
    slot<void (*)(Object*, std::uint32_t, std::uint32_t, std::uint32_t)>(commands, 0x70)(commands, 1, 1, 1);
    CHECK(draw::dispatched() == before + 1);

    D3D12_HEAP_PROPERTIES colour_heap{};
    colour_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC colour_desc{};
    colour_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colour_desc.Width = kWidth;
    colour_desc.Height = 1;
    colour_desc.DepthOrArraySize = 1;
    colour_desc.MipLevels = 1;
    colour_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colour_desc.SampleDesc = {1, 0};
    colour_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE colour_clear{};
    colour_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Microsoft::WRL::ComPtr<ID3D12Resource> colour;
    CHECK(SUCCEEDED(gpu->device()->CreateCommittedResource(&colour_heap, D3D12_HEAP_FLAG_NONE, &colour_desc,
                                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &colour_clear,
                                                           IID_PPV_ARGS(&colour))));
    const Microsoft::WRL::ComPtr<ID3D12RootSignature> graphics_root = empty_root(gpu->device());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature = graphics_root.Get();
    graphics.VS = {g_depth_equal_vs, sizeof g_depth_equal_vs};
    graphics.PS = {g_depth_equal_ps, sizeof g_depth_equal_ps};
    graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable = TRUE;
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.DepthStencilState.DepthEnable = TRUE;
    graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    graphics.SampleMask = UINT_MAX;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics.DSVFormat = DXGI_FORMAT_D16_UNORM;
    graphics.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> equal;
    CHECK(SUCCEEDED(gpu->device()->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&equal))));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{};
    rtv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap.NumDescriptors = 1;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap{};
    dsv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap.NumDescriptors = 1;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvs;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvs;
    CHECK(SUCCEEDED(gpu->device()->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&rtvs))));
    CHECK(SUCCEEDED(gpu->device()->CreateDescriptorHeap(&dsv_heap, IID_PPV_ARGS(&dsvs))));
    if (colour && equal && rtvs && dsvs) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
        gpu->device()->CreateRenderTargetView(colour.Get(), nullptr, rtv);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D16_UNORM;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        gpu->device()->CreateDepthStencilView(host_depth, &dsv_desc, dsv);
        const float black[4] = {};
        recorded->list()->ClearRenderTargetView(rtv, black, 0, nullptr);
        recorded->list()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        recorded->list()->SetGraphicsRootSignature(graphics_root.Get());
        recorded->list()->SetPipelineState(equal.Get());
        recorded->list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(kWidth);
        viewport.Height = 1;
        viewport.MaxDepth = 1;
        recorded->list()->RSSetViewports(1, &viewport);
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), 1};
        recorded->list()->RSSetScissorRects(1, &scissor);
        recorded->list()->DrawInstanced(3, 1, 0, 0);
    }
    CHECK(slot<HRESULT (*)(Object*)>(commands, 0x48)(commands) == S_OK);
    const queue::Desc queue_desc{0, 0, 0, 1};
    Object* command_queue = nullptr;
    CHECK(queue::create(nullptr, &queue_desc, &queue::kIids[0], &command_queue) == S_OK);
    Object* submitted[1] = {commands};
    slot<void (*)(Object*, std::uint32_t, Object* const*)>(command_queue, 0x50)(command_queue, 1, submitted);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_foot{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC depth_host = host_depth->GetDesc();
    gpu->device()->GetCopyableFootprints(&depth_host, 0, 1, 0, &depth_foot, &rows, &row_bytes, &total);
    const Microsoft::WRL::ComPtr<ID3D12Resource> depth_read = readback(gpu->device(), total);
    const auto depth_state = texture::host_transition(depth, 0x800);
    auto reader = gpu->new_list();
    CHECK(reader != nullptr);
    if (reader && depth_read) {
        if (depth_state) {
            const D3D12_RESOURCE_BARRIER barrier =
                host::transition(host_depth, static_cast<D3D12_RESOURCE_STATES>(depth_state->second),
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
            reader->list()->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = depth_read.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = depth_foot;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = host_depth;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        reader->list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (colour) {
            D3D12_RESOURCE_BARRIER colour_barrier =
                host::transition(colour.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            reader->list()->ResourceBarrier(1, &colour_barrier);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT colour_foot{};
            UINT64 colour_total = 0;
            const D3D12_RESOURCE_DESC colour_host = colour->GetDesc();
            gpu->device()->GetCopyableFootprints(&colour_host, 0, 1, 0, &colour_foot, &rows, &row_bytes,
                                                 &colour_total);
            Microsoft::WRL::ComPtr<ID3D12Resource> colour_read = readback(gpu->device(), colour_total);
            if (colour_read) {
                to.pResource = colour_read.Get();
                to.PlacedFootprint = colour_foot;
                from.pResource = colour.Get();
                reader->list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                reader->close();
                gpu->execute({reader.get()});
                void* mapped = nullptr;
                const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
                CHECK(SUCCEEDED(depth_read->Map(0, &range, &mapped)));
                if (mapped != nullptr) {
                    const auto* pixels = reinterpret_cast<const std::uint16_t*>(
                        static_cast<const std::uint8_t*>(mapped) + depth_foot.Offset);
                    CHECK(pixels[0] == kFar);
                    CHECK(pixels[1] == kHit);
                    CHECK(pixels[2] == kFar);
                    CHECK(pixels[3] == kFar);
                    depth_read->Unmap(0, nullptr);
                }
                void* colour_mapped = nullptr;
                const D3D12_RANGE colour_range{0, static_cast<SIZE_T>(colour_total)};
                CHECK(SUCCEEDED(colour_read->Map(0, &colour_range, &colour_mapped)));
                if (colour_mapped != nullptr) {
                    const auto* bytes = static_cast<const std::uint8_t*>(colour_mapped) + colour_foot.Offset;
                    CHECK(bytes[0] == 0);
                    CHECK(bytes[4] == 255);
                    CHECK(bytes[8] == 0);
                    colour_read->Unmap(0, nullptr);
                }
            }
        }
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    drop_object(command_queue);
    drop_object(commands);
    drop_object(allocator);
    drop_object(heap);
    drop_object(depth);
    drop_object(title_pipeline);
}
