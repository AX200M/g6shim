// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "upscaler_loader/translate.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace u = g6shim::upscaler;

namespace {

constexpr u::StructType kConsoleBackendDevice = u::kBackendConsole | u::kBackendDevice;
constexpr u::StructType kConsoleOnly = u::kBackendConsole | 0x99u;
constexpr std::uint32_t kUpscalerVersion = (4u << 22) | (1u << 12) | 2u;

struct TitleChain {
    u::CreateUpscale upscale{};
    u::BackendDevice backend{};
    u::Header console_only{};
    u::CreateUpscaleVersion version{};

    TitleChain(std::uint32_t render_width, std::uint32_t render_height, std::uint32_t width, std::uint32_t height) {
        upscale.header.type = u::kCreateUpscale;
        upscale.max_render = {render_width, render_height};
        upscale.max_upscale = {width, height};
        backend.header.type = kConsoleBackendDevice;
        backend.device = reinterpret_cast<void*>(std::uintptr_t{0x1234});
        console_only.type = kConsoleOnly;
        version.header.type = u::kCreateUpscaleVersion;
        version.version = kUpscalerVersion;
        upscale.header.next = &backend.header;
        backend.header.next = &console_only;
        console_only.next = &version.header;
    }
};

}  // namespace

TEST(upscaler_create_chain_swaps_the_console_backend_for_the_host_device) {
    TitleChain title(640, 360, 1280, 720);
    void* host_device = reinterpret_cast<void*>(std::uintptr_t{0xabcd});
    u::CreateChain chain = u::translate_create(&title.upscale.header, host_device);
    CHECK(chain.has_upscale);
    CHECK(chain.has_version);
    CHECK(chain.upscale.max_render.width == 640);
    CHECK(chain.upscale.max_upscale.height == 720);
    CHECK(chain.version.version == kUpscalerVersion);
    CHECK(chain.backend.header.type == (u::kBackendDx12 | u::kBackendDevice));
    CHECK(chain.backend.device == host_device);
    CHECK(chain.dropped.size() == 1);
    CHECK(!chain.dropped.empty() && chain.dropped[0] == kConsoleOnly);

    u::Header* with = chain.head(true);
    CHECK(with == &chain.upscale.header);
    CHECK(with->next == &chain.version.header);
    CHECK(chain.version.header.next == &chain.backend.header);
    CHECK(chain.backend.header.next == nullptr);
    u::Header* without = chain.head(false);
    CHECK(without->next == &chain.backend.header);
    CHECK(chain.backend.header.next == nullptr);
    CHECK(title.backend.device == reinterpret_cast<void*>(std::uintptr_t{0x1234}));
}

TEST(upscaler_create_chain_without_an_upscale_desc_has_no_head) {
    u::BackendDevice backend{};
    backend.header.type = kConsoleBackendDevice;
    u::CreateChain chain = u::translate_create(&backend.header, nullptr);
    CHECK(!chain.has_upscale);
    CHECK(chain.head(true) == nullptr);
}

TEST(upscaler_resource_states_map_only_exact_shader_access) {
    CHECK(u::resource_state(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ==
          static_cast<std::uint32_t>(u::State::kComputeRead));
    CHECK(u::resource_state(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ==
          static_cast<std::uint32_t>(u::State::kUnorderedAccess));
    CHECK(!u::resource_state(D3D12_RESOURCE_STATE_COMMON));
    CHECK(!u::resource_state(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}

TEST(upscaler_descriptions_follow_the_host_texture) {
    D3D12_RESOURCE_DESC host{};
    host.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    host.Width = 1920;
    host.Height = 1080;
    host.DepthOrArraySize = 1;
    host.MipLevels = 1;
    host.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    host.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    u::ResourceDescription title{};
    title.format = static_cast<std::uint32_t>(u::SurfaceFormat::kR8G8B8A8Unorm);
    title.width = 7;
    u::ResourceDescription out = u::describe(host, title);
    CHECK(out.width == 1920);
    CHECK(out.height == 1080);
    CHECK(out.type == static_cast<std::uint32_t>(u::ResourceType::kTexture2D));
    CHECK(out.format == static_cast<std::uint32_t>(u::SurfaceFormat::kR8G8B8A8Unorm));
    CHECK((out.usage & u::kUsageUnorderedAccess) != 0);

    host.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    host.Flags = D3D12_RESOURCE_FLAG_NONE;
    title.usage = u::kUsageUnorderedAccess;
    out = u::describe(host, title);
    CHECK(out.format == static_cast<std::uint32_t>(u::SurfaceFormat::kB8G8R8A8Unorm));
    CHECK((out.usage & u::kUsageUnorderedAccess) == 0);

    host.Format = DXGI_FORMAT_R32_TYPELESS;
    title.format = static_cast<std::uint32_t>(u::SurfaceFormat::kUnknown);
    out = u::describe(host, title);
    CHECK(out.format == static_cast<std::uint32_t>(u::SurfaceFormat::kR32Typeless));
}

namespace {

Microsoft::WRL::ComPtr<ID3D12Resource> texture(ID3D12Device* device, std::uint32_t width, std::uint32_t height,
                                               DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    Microsoft::WRL::ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
                                               IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER out{};
    out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out.Transition.pResource = resource;
    out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out.Transition.StateBefore = before;
    out.Transition.StateAfter = after;
    return out;
}

u::Resource hosted(ID3D12Resource* resource, std::uint32_t state) {
    u::Resource out{};
    out.resource = resource;
    out.description = u::describe(resource->GetDesc(), u::ResourceDescription{});
    out.state = state;
    return out;
}

}  // namespace

TEST(upscaler_host_provider_upscales_through_the_translated_chain) {
    wchar_t folder[MAX_PATH] = {};
    const DWORD length = ::GetEnvironmentVariableW(L"G6SHIM_TEST_UPSCALER_DIR", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    const std::wstring path = std::wstring(folder) + L"\\amd_fidelityfx_loader_dx12.dll";
    HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(module != nullptr);
    if (module == nullptr) return;
    const auto create = reinterpret_cast<u::CreateContextFn>(::GetProcAddress(module, "ffxCreateContext"));
    const auto destroy = reinterpret_cast<u::DestroyContextFn>(::GetProcAddress(module, "ffxDestroyContext"));
    const auto query = reinterpret_cast<u::QueryFn>(::GetProcAddress(module, "ffxQuery"));
    const auto dispatch = reinterpret_cast<u::DispatchFn>(::GetProcAddress(module, "ffxDispatch"));
    CHECK(create && destroy && query && dispatch);
    if (!create || !destroy || !query || !dispatch) return;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    CHECK(SUCCEEDED(::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));
    if (!device) return;

    constexpr std::uint32_t kRenderWidth = 640;
    constexpr std::uint32_t kRenderHeight = 360;
    constexpr std::uint32_t kWidth = 1280;
    constexpr std::uint32_t kHeight = 720;
    TitleChain title(kRenderWidth, kRenderHeight, kWidth, kHeight);
    u::CreateChain chain = u::translate_create(&title.upscale.header, device.Get());
    u::Context context = nullptr;
    u::ReturnCode made = create(&context, chain.head(true), nullptr);
    if (made != u::kOk) made = create(&context, chain.head(false), nullptr);
    CHECK(made == u::kOk);
    if (made != u::kOk) return;
    u::QueryProviderVersion provider{};
    provider.header.type = u::kQueryProviderVersion;
    CHECK(query(&context, &provider.header) == u::kOk);
    CHECK(provider.name != nullptr);
    if (provider.name != nullptr) std::printf("upscaler test: host provider %s\n", provider.name);

    const float kGray[4] = {0.5f, 0.25f, 0.125f, 1.0f};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::memcpy(clear.Color, kGray, sizeof kGray);
    auto color = texture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    auto depth = texture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    auto motion = texture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R16G16_FLOAT,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    auto output = texture(device.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    CHECK(color && depth && motion && output);
    if (!color || !depth || !motion || !output) {
        destroy(&context, nullptr);
        return;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> targets;
    CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&targets))));
    const D3D12_CPU_DESCRIPTOR_HANDLE target = targets->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.Get(), nullptr, target);
    list->ClearRenderTargetView(target, kGray, 0, nullptr);
    const D3D12_RESOURCE_BARRIER to_read = barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);

    const std::uint32_t kRead = static_cast<std::uint32_t>(u::State::kComputeRead);
    u::DispatchUpscale work{};
    work.header.type = u::kDispatchUpscale;
    work.command_list = list.Get();
    work.color = hosted(color.Get(), kRead);
    work.depth = hosted(depth.Get(), kRead);
    work.motion_vectors = hosted(motion.Get(), kRead);
    work.output = hosted(output.Get(), static_cast<std::uint32_t>(u::State::kUnorderedAccess));
    work.motion_vector_scale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
    work.render_size = {kRenderWidth, kRenderHeight};
    work.upscale_size = {kWidth, kHeight};
    work.frame_time_delta = 16.6f;
    work.pre_exposure = 1.0f;
    work.reset = true;
    work.camera_near = 0.1f;
    work.camera_far = 1000.0f;
    work.camera_fov = 1.0f;
    work.meters_per_unit = 1.0f;
    CHECK(dispatch(&context, &work.header) == u::kOk);

    const D3D12_RESOURCE_BARRIER to_copy = barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
    device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback))));
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = output.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE done = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, done);
    CHECK(::WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
    ::CloseHandle(done);
    CHECK(device->GetDeviceRemovedReason() == S_OK);

    void* mapped = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    if (mapped != nullptr) {
        const auto* row = static_cast<const std::uint8_t*>(mapped) +
                          static_cast<std::size_t>(footprint.Footprint.RowPitch) * (kHeight / 2);
        std::uint16_t pixel[4] = {};
        std::memcpy(pixel, row + (kWidth / 2) * sizeof pixel, sizeof pixel);
        std::printf("upscaler test: output center %04x %04x %04x %04x\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        CHECK(pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0);
        readback->Unmap(0, nullptr);
    }
    CHECK(destroy(&context, nullptr) == u::kOk);
}
