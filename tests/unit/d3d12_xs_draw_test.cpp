// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/draw.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/list.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/pipeline.hpp"
#include "d3d12_xs/pipeline_bridge.hpp"
#include "d3d12_xs/pipeline_host.hpp"
#include "d3d12_xs/queue.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/texture.hpp"

#include "g6shim/shader/emit/container.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

struct Environment {
    Environment() {
        ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) == 0) return;
        const std::filesystem::path path = std::filesystem::path(temp) / L"g6shim_draw_test";
        ::SetEnvironmentVariableW(L"G6SHIM_CACHE_DIR", path.c_str());
    }
};

const Environment g_environment;

std::vector<std::uint8_t> synthetic_compute_container() {
    constexpr std::uint32_t kKindNative = 0x0D;
    constexpr std::uint32_t kXbchVersion = 3;
    constexpr std::uint32_t kFssmVersion = 0x101;
    constexpr std::size_t kXdatOffset = 0x18;
    constexpr std::uint64_t kGraphicsSlotCount = 6;
    constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFULL;
    constexpr std::size_t kRecordSize = 0x54;
    constexpr std::size_t kRecordFlags = 0x14;
    constexpr std::size_t kRecordRegs = 0x50;
    constexpr std::size_t kCsSlot = 0x60;
    constexpr std::size_t kStageBytes = 0x38;
    constexpr std::size_t kCsThreads = 0x28;
    constexpr std::uint32_t kEndpgm = 0xBF81'0000;
    constexpr std::array<std::size_t, 8> kSlots{0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60};
    const auto append32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto append64 = [&append32](std::vector<std::uint8_t>& out, std::uint64_t value) {
        append32(out, static_cast<std::uint32_t>(value));
        append32(out, static_cast<std::uint32_t>(value >> 32));
    };
    const auto set32 = [](std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto set64 = [&set32](std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
        set32(out, at, static_cast<std::uint32_t>(value));
        set32(out, at + 4, static_cast<std::uint32_t>(value >> 32));
    };
    const auto adler = [](const std::vector<std::uint8_t>& data) {
        constexpr std::uint32_t kMod = 65521;
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const std::uint8_t byte : data) {
            a = (a + byte) % kMod;
            b = (b + a) % kMod;
        }
        return (b << 16) | a;
    };
    const auto zlib = [&adler](const std::vector<std::uint8_t>& data) {
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
        for (const std::uint32_t shift : {24u, 16u, 8u, 0u})
            out.push_back(static_cast<std::uint8_t>(check >> shift));
        return out;
    };
    const auto xbch = [&zlib, &append32](const std::vector<std::uint8_t>& unpacked) {
        const std::vector<std::uint8_t> stream = zlib(unpacked);
        std::vector<std::uint8_t> out{'X', 'B', 'C', 'H'};
        append32(out, kXbchVersion);
        for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(stream.size() >> (8 * i)));
        for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(unpacked.size() >> (8 * i)));
        out.insert(out.end(), stream.begin(), stream.end());
        return out;
    };
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
    payload.resize(static_cast<std::size_t>(record) + kRecordRegs, 0);
    append32(payload, 28 | 31U << 20);
    payload.resize(static_cast<std::size_t>(record) + kRecordSize, 0);
    payload.insert(payload.end(), stage.begin(), stage.end());
    payload.insert(payload.end(), native_code.begin(), native_code.end());
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

resource::ResourceDesc texture_desc() {
    resource::ResourceDesc desc{};
    desc.dimension = 3;
    desc.width = 64;
    desc.height = 64;
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
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

}  // namespace

TEST(d3d12_xs_list_dispatches_through_a_host_pipeline) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    const std::vector<std::uint8_t> container = synthetic_compute_container();
    pipeline::ComputeHead description{};
    description.stage = pipeline::Bytecode{container.data(), container.size()};
    Object* title_pipeline = nullptr;
    CHECK(pipeline::create_compute(&description, &pipeline::kIids[0], &title_pipeline) == S_OK);
    CHECK(title_pipeline != nullptr);
    if (title_pipeline == nullptr) return;
    pipeline_host::HostPipeline* host_pipeline = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (host_pipeline == nullptr && std::chrono::steady_clock::now() < deadline) {
        host_pipeline = pipeline_bridge::host_pipeline_for(title_pipeline, true);
        if (host_pipeline == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(host_pipeline != nullptr);
    if (host_pipeline == nullptr) return;
    CHECK(host_pipeline->compute);
    CHECK(!host_pipeline->mesh);

    const resource::ResourceDesc target_desc = texture_desc();
    constexpr std::uint32_t kRenderTargetState = 0x4;
    const resource::HeapProperties properties{1, 0, 0, 1, 1};
    Object* target = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &target_desc, kRenderTargetState, nullptr,
                                     &resource::kResourceIids[0], &target) == S_OK);
    CHECK(target != nullptr);
    if (target == nullptr) return;
    ID3D12Resource* host_target = texture::host_texture(target);
    CHECK(host_target != nullptr);
    if (host_target == nullptr) return;

    const descriptor::HeapDesc heap_desc{2, 1, 0, 0};
    Object* heap = nullptr;
    CHECK(descriptor::create_heap(nullptr, &heap_desc, &descriptor::kIids[0], &heap) == S_OK);
    CHECK(heap != nullptr);
    if (heap == nullptr) return;
    const std::uint64_t view = static_cast<descriptor::Heap*>(heap)->start;
    descriptor::create_rtv(nullptr, target, nullptr, view);

    Object* allocator = nullptr;
    CHECK(list::create_allocator(nullptr, 0, &list::kAllocatorIids[0], &allocator) == S_OK);
    Object* commands = nullptr;
    CHECK(list::create_list(nullptr, 1, 0, allocator, nullptr, &list::kListIids[0], &commands) == S_OK);
    CHECK(commands != nullptr);
    if (commands == nullptr) return;

    const std::array<float, 4> clear{0.4f, 0.2f, 0.8f, 1.0f};
    const auto clear_target =
        slot<void (*)(Object*, std::uint64_t, const float*, std::uint32_t, const RECT*)>(commands, 0x1a0);
    clear_target(commands, view, clear.data(), 0, nullptr);
    slot<void (*)(Object*, Object*)>(commands, 0xc8)(commands, title_pipeline);
    const std::uint64_t before = draw::dispatched();
    slot<void (*)(Object*, std::uint32_t, std::uint32_t, std::uint32_t)>(commands, 0x70)(commands, 1, 1, 1);
    CHECK(draw::dispatched() == before + 1);
    CHECK(slot<HRESULT (*)(Object*)>(commands, 0x48)(commands) == S_OK);

    const queue::Desc queue_desc{0, 0, 0, 1};
    Object* command_queue = nullptr;
    CHECK(queue::create(nullptr, &queue_desc, &queue::kIids[0], &command_queue) == S_OK);
    CHECK(command_queue != nullptr);
    if (command_queue == nullptr) return;
    Object* submitted[1] = {commands};
    slot<void (*)(Object*, std::uint32_t, Object* const*)>(command_queue, 0x50)(command_queue, 1, submitted);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC host_desc = host_target->GetDesc();
    gpu->device()->GetCopyableFootprints(&host_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
    CHECK(total > 0);
    const Microsoft::WRL::ComPtr<ID3D12Resource> readback = readback_buffer(gpu->device(), total);
    CHECK(readback != nullptr);
    if (readback == nullptr) return;

    const auto changed = texture::host_transition(target, 0x800);
    auto reader = gpu->new_list();
    CHECK(reader != nullptr);
    if (reader != nullptr) {
        if (changed) {
            const D3D12_RESOURCE_BARRIER barrier =
                host::transition(host_target, static_cast<D3D12_RESOURCE_STATES>(changed->second),
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
            reader->list()->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = host_target;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        reader->list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        reader->close();
        gpu->execute({reader.get()});
        CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        CHECK(SUCCEEDED(readback->Map(0, &range, &mapped)));
        if (mapped != nullptr) {
            const auto* pixel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
            CHECK(pixel[0] == 102);
            CHECK(pixel[1] == 51);
            CHECK(pixel[2] == 204);
            CHECK(pixel[3] == 255);
            readback->Unmap(0, nullptr);
        }
    }

    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    CHECK(slot<std::uint32_t (*)(Object*)>(command_queue, 0x10)(command_queue) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(allocator, 0x10)(allocator) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(target, 0x10)(target) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(title_pipeline, 0x10)(title_pipeline) == 0);
}
