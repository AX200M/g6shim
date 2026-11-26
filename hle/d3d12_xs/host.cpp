// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.hpp"

#include "g_present_plane_ps.h"
#include "g_present_plane_vs.h"
#include "queue.hpp"
#include "resource.hpp"
#include "shadow.hpp"
#include "texture.hpp"
#include "watch.hpp"
#include "window.hpp"

#include <g6shim/capture.hpp>
#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>
#include <g6shim/window.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

namespace g6shim::d3d12_xs::host {

namespace {

constexpr std::uint32_t kSwapChainBuffers = 2;
constexpr std::uint32_t kPresentInterval = 0;
constexpr std::size_t kPieceBytes = 64 << 10;
constexpr std::size_t kFinePieceBytes = 4 << 10;
constexpr DWORD kDeviceRemoved = 0x887A0005u;


std::optional<DXGI_FORMAT> swap_format(int format) {
    if (format >= 27 && format <= 32) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == 87 || format == 90 || format == 91) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format >= 23 && format <= 25) return DXGI_FORMAT_R10G10B10A2_UNORM;
    if (format >= 9 && format <= 14) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return std::nullopt;
}

struct Capture {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    PixelOrder order = PixelOrder::Bgra;
};

std::optional<PixelOrder> capture_order(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return PixelOrder::Rgba;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return PixelOrder::Bgra;
    default: return std::nullopt;
    }
}

bool begin_capture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                   const D3D12_RESOURCE_DESC& desc, Capture& capture) {
    const std::optional<PixelOrder> order = capture_order(desc.Format);
    if (!order) {
        G6SHIM_WARN("d3d12_xs: capture: back buffer format %d not supported", static_cast<int>(desc.Format));
        return false;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.Width == 0 ||
        desc.Height == 0) {
        G6SHIM_WARN("d3d12_xs: capture: back buffer is not a single-sampled 2D texture");
        return false;
    }
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &capture.footprint, nullptr, nullptr, &bytes);
    if (bytes == 0) {
        G6SHIM_WARN("d3d12_xs: capture: back buffer has no copyable footprint");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&capture.readback));
    if (FAILED(made)) {
        G6SHIM_WARN("d3d12_xs: capture: no readback buffer of %llu bytes (0x%08lX)",
                    static_cast<unsigned long long>(bytes), static_cast<unsigned long>(made));
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destination{capture.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = capture.footprint;
    D3D12_TEXTURE_COPY_LOCATION source{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    capture.order = *order;
    return true;
}

void finish_capture(unsigned frame, const D3D12_RESOURCE_DESC& desc, Capture& capture) {
    void* data = nullptr;
    const HRESULT mapped = capture.readback->Map(0, nullptr, &data);
    if (FAILED(mapped)) {
        G6SHIM_WARN("d3d12_xs: capture: readback not mapped (0x%08lX)", static_cast<unsigned long>(mapped));
        return;
    }
    save_captured_frame(frame, data, static_cast<unsigned>(desc.Width), desc.Height,
                        capture.footprint.Footprint.RowPitch, capture.order);
    capture.readback->Unmap(0, nullptr);
}

bool read_process(const std::uint8_t* source, std::uint8_t* destination, std::size_t bytes) {
    SIZE_T done = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), source, destination, bytes, &done) != 0;
}

bool write_process(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes) {
    SIZE_T done = 0;
    return ::WriteProcessMemory(::GetCurrentProcess(), destination, source, bytes, &done) != 0;
}

template <typename Work>
void for_pieces(std::size_t start, std::size_t bytes, std::size_t step, Work work) {
    const std::size_t end = start + bytes;
    std::size_t at = start;
    while (at < end) {
        const std::size_t next = std::min((at / step + 1) * step, end);
        work(at, next - at);
        at = next;
    }
}

struct Meter {
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    std::uint32_t frames = 0;
    std::uint64_t evicted = 0;
    std::chrono::steady_clock::time_point logged = std::chrono::steady_clock::now();

    void frame(HWND hwnd);
};

void Meter::frame(HWND hwnd) {
    ++frames;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - since;
    if (elapsed.count() < 1.0) return;
    const double seconds = elapsed.count();
    const double fps = frames / seconds;
    const double ms = seconds * 1000.0 / frames;
    if (std::chrono::duration<double>(now - logged).count() >= 5.0) {
        logged = now;
        const shadow::LooseStats loose = shadow::loose_stats();
        G6SHIM_INFO("d3d12_xs: %.1f fps, %.1f ms/frame, loose title memory %zu windows %llu MiB, %llu evicted", fps,
                    ms, loose.windows, static_cast<unsigned long long>(loose.bytes >> 20),
                    static_cast<unsigned long long>(loose.evicted - evicted));
        evicted = loose.evicted;
    }
    window::show_frame_rate(hwnd, window::FrameRate{fps, ms});
    frames = 0;
    since = now;
}

BOOL CALLBACK consider_window(HWND window, LPARAM found) {
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    if (process == ::GetCurrentProcessId() && ::IsWindowVisible(window)) {
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

HWND title_window() {
    if (auto* window = static_cast<HWND>(g6shim::main_window())) return window;
    HWND found = nullptr;
    ::EnumWindows(&consider_window, reinterpret_cast<LPARAM>(&found));
    return found;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

enum class PresenterFailure : std::uint8_t { kWindow, kFactory, kSwapChain, kQuery, kCopy, kCount };

void log_presenter_failure(PresenterFailure which, const char* what, std::optional<HRESULT> hr = std::nullopt) {
    static std::array<std::once_flag, static_cast<std::size_t>(PresenterFailure::kCount)> logged;
    std::call_once(logged[static_cast<std::size_t>(which)], [what, hr] {
        if (hr)
            G6SHIM_WARN("d3d12_xs: presentation could not start: %s (0x%08lX)", what, static_cast<unsigned long>(*hr));
        else
            G6SHIM_WARN("d3d12_xs: presentation could not start: %s", what);
    });
}

void log_presentation_skipped() {
    static std::once_flag logged;
    std::call_once(logged, [] { G6SHIM_WARN("d3d12_xs: presentation skipped, no presenter"); });
}

}  // namespace

std::uint32_t pc_state(std::uint32_t state) { return state & kPcStateMask; }

std::uint32_t texture_state(std::uint32_t state) { return pc_state(state) & ~kBufferOnlyStates; }

std::uint32_t legal_for(std::uint32_t flags, std::uint32_t state) {
    constexpr std::array<std::tuple<std::uint32_t, bool, std::uint32_t>, 4> kForbidden{{
        {0x1, false, 0x4},
        {0x4, false, 0x8},
        {0x2, false, 0x10 | 0x20},
        {0x8, true, 0x40 | 0x80},
    }};
    for (const auto& [flag, deny, bits] : kForbidden)
        if (((flags & flag) != 0) == deny) state &= ~bits;
    return state;
}

std::uint32_t legal_state(ID3D12Resource* texture, std::uint32_t state) {
    if (texture == nullptr) return state;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    return legal_for(static_cast<std::uint32_t>(desc.Flags), state);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

Written Written::of(std::uint64_t value) {
    Written written;
    written.kind = Kind::kValue;
    written.value = value;
    return written;
}

Written Written::gpu_timestamp() {
    Written written;
    written.kind = Kind::kGpuTimestamp;
    return written;
}

Written Written::system_timestamp() {
    Written written;
    written.kind = Kind::kSystemTimestamp;
    return written;
}

void Written::write(std::uint8_t* at, std::size_t bytes) const {
    if (at == nullptr) return;
    std::uint64_t stored = value;
    if (kind == Kind::kGpuTimestamp) {
        static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        stored = static_cast<std::uint64_t>(nanos / (1'000'000'000 / kTimestampFrequency));
    } else if (kind == Kind::kSystemTimestamp) {
        LARGE_INTEGER counter{};
        ::QueryPerformanceCounter(&counter);
        stored = static_cast<std::uint64_t>(counter.QuadPart);
    }
    if (bytes == 4) {
        const auto value32 = static_cast<std::uint32_t>(stored);
        std::memcpy(at, &value32, sizeof value32);
    } else {
        std::memcpy(at, &stored, sizeof stored);
    }
}

void Batch::add(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address, std::size_t bytes) {
    const std::size_t end = address + bytes;
    for (auto& [entry, from, to] : refreshes_) {
        if (entry.get() == shadow.get()) {
            from = std::min(from, address);
            to = std::max(to, end);
            return;
        }
    }
    refreshes_.emplace_back(shadow, address, end);
}

bool Batch::texture_due(const void* staging) { return textures_.insert(staging).second; }

void Batch::title_written() {
    flush();
    textures_.clear();
}

void Batch::flush() {
    for (const auto& [shadow_entry, from, to] : refreshes_) shadow_entry->update(from, to - from);
    refreshes_.clear();
}

Host::Host() {
    const bool debug = settings::enabled("G6SHIM_D3D_DEBUG");
    if (debug) {
        Microsoft::WRL::ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            layer->EnableDebugLayer();
        else
            G6SHIM_WARN("d3d12_xs: no host D3D12 debug layer");
    }
    HRESULT made = E_FAIL;
    if (settings::enabled("G6SHIM_D3D_WARP")) {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> listed;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter.As(&listed)) && SUCCEEDED(listed->GetDesc1(&desc)) &&
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                made = ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
            } else {
                G6SHIM_WARN("d3d12_xs: DXGI did not identify the software adapter; no host device");
            }
        }
    } else {
        made = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
    }
    if (FAILED(made) || !device_) {
        G6SHIM_WARN("d3d12_xs: no host D3D12 device (0x%08lX); command lists will not run",
                    static_cast<unsigned long>(made));
        return;
    }
    if (debug) device_.As(&messages_);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)))) {
        G6SHIM_WARN("d3d12_xs: no host command queue");
        device_.Reset();
        return;
    }
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        G6SHIM_WARN("d3d12_xs: no host fence");
        device_.Reset();
        return;
    }
    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr) {
        G6SHIM_WARN("d3d12_xs: no host submission event");
        device_.Reset();
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kRtvRing;
    if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs_)))) {
        G6SHIM_WARN("d3d12_xs: no host render target heap");
        device_.Reset();
        return;
    }
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsvs_)))) {
        G6SHIM_WARN("d3d12_xs: no host depth heap");
        device_.Reset();
        return;
    }
    dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    G6SHIM_INFO("d3d12_xs: host D3D12 device ready");
}

struct Host::Presenter {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::unique_ptr<HostList> copy;
    HWND hwnd = nullptr;
    Meter meter;
};

namespace {

constexpr std::uint32_t kPresentPipelineSlots = 4;

static_assert(queue::kMaxPresentedPlanes == 4);

DXGI_FORMAT colour_view_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    default: return format;
    }
}

bool make_compose_root(ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12RootSignature>& root) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = queue::kMaxPresentedPlanes;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 2;
    signature.pParameters = parameters;
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(),
                                           error.GetAddressOf())))
        return false;
    return SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&root)));
}

bool make_compose_pipeline(ID3D12Device* device, ID3D12RootSignature* root, DXGI_FORMAT format,
                           Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS.pShaderBytecode = g_present_plane_vs;
    desc.VS.BytecodeLength = sizeof(g_present_plane_vs);
    desc.PS.pShaderBytecode = g_present_plane_ps;
    desc.PS.BytecodeLength = sizeof(g_present_plane_ps);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
}

bool sampleable(ID3D12Resource* texture) {
    if (texture == nullptr) return false;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.Width != 0 &&
           desc.Height != 0;
}

struct BarrierBatch {
    std::array<D3D12_RESOURCE_BARRIER, queue::kMaxPresentedPlanes + 1> barriers{};
    UINT count = 0;

    void add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (resource == nullptr || before == after || count >= barriers.size()) return;
        barriers[count++] = transition(resource, before, after);
    }

    void record(ID3D12GraphicsCommandList* list) {
        if (count == 0) return;
        list->ResourceBarrier(count, barriers.data());
        count = 0;
    }
};

}  // namespace

struct Host::Composer {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views;
    std::uint32_t increment = 0;
    std::uint32_t ring = 0;
    struct Pipeline {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
        bool failed = false;
    };
    std::array<Pipeline, kPresentPipelineSlots> pipelines{};
};

Host::~Host() = default;

Host* Host::get() {
    static Host* instance = []() -> Host* {
        std::unique_ptr<Host> made(new Host());
        if (!made->device_) return nullptr;
        return made.release();
    }();
    return instance;
}

Host* host() { return Host::get(); }

std::unique_ptr<HostList> Host::new_list() {
    if (!device_) return nullptr;
    std::unique_ptr<HostList> made(new HostList());
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&made->allocator_))))
        return nullptr;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made->allocator_.Get(), nullptr,
                                          IID_PPV_ARGS(&made->list_))))
        return nullptr;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&made->prelude_allocator_))))
        return nullptr;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made->prelude_allocator_.Get(),
                                          nullptr, IID_PPV_ARGS(&made->prelude_))))
        return nullptr;
    made->prelude_->Close();
    return made;
}

void Host::execute(const std::vector<HostList*>& runs) {
    std::lock_guard<std::mutex> submitted_lock(submitted_mutex_);
    Batch batch;
    std::vector<ID3D12CommandList*> lists;
    lists.reserve(runs.size() * 2);
    for (HostList* list : runs) {
        list->run_deferred(batch);
        if (ID3D12CommandList* prelude = list->prelude()) lists.push_back(prelude);
        lists.push_back(list->list());
    }
    batch.flush();
    ++submitted_;
    queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    const HRESULT signalled = queue_->Signal(fence_.Get(), submitted_);
    bool waited = false;
    if (SUCCEEDED(signalled) && SUCCEEDED(fence_->SetEventOnCompletion(submitted_, event_))) {
        waited = ::WaitForSingleObject(event_, INFINITE) == WAIT_OBJECT_0;
    } else {
        G6SHIM_WARN("d3d12_xs: host submission not waited for (0x%08lX)", static_cast<unsigned long>(signalled));
    }
    if (FAILED(device_->GetDeviceRemovedReason()))
        fail("the host GPU device was removed after a submission");
    std::vector<std::tuple<std::shared_ptr<shadow::Shadow>, std::size_t, std::size_t>> back;
    for (HostList* list : runs) {
        std::lock_guard<std::mutex> lock(list->deferred_mutex_);
        for (const Step& step : list->deferred_.copies) {
            const auto* write = std::get_if<GpuWriteStep>(&step);
            if (write == nullptr) continue;
            const bool known = std::any_of(back.begin(), back.end(), [&](const auto& entry) {
                return std::get<0>(entry).get() == write->shadow.get() && std::get<1>(entry) == write->address &&
                       std::get<2>(entry) == write->bytes;
            });
            if (!known) back.emplace_back(write->shadow, write->address, write->bytes);
        }
    }
    for (const auto& [copy, address, bytes] : back) copy->copy_back(address, bytes);
    for (HostList* list : runs) {
        std::lock_guard<std::mutex> lock(list->deferred_mutex_);
        for (const WriteStep& write : list->deferred_.writes) write.written.write(write.destination, write.bytes);
    }
    log_messages();
    if (waited) shadow::submission_completed();
}

std::optional<std::string> Host::log_messages() {
    if (!messages_) return std::nullopt;
    std::optional<std::string> first_error;
    const UINT64 count = messages_->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(messages_->GetMessage(static_cast<UINT>(i), nullptr, &size)) || size == 0) continue;
        std::vector<std::uint64_t> storage((size + 7) / 8, 0);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(messages_->GetMessage(static_cast<UINT>(i), message, &size))) continue;
        std::string text(message->pDescription, message->DescriptionByteLength);
        while (!text.empty() && text.back() == '\0') text.pop_back();
        G6SHIM_WARN("d3d12_xs: host D3D12 (%d, %d): %s", static_cast<int>(message->ID),
                    static_cast<int>(message->Severity), text.c_str());
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && !first_error) first_error = text;
    }
    messages_->ClearStoredMessages();
    return first_error;
}

bool Host::mesh_shaders() {
    static const bool supported = [this] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
        const bool queried =
            SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof options)) &&
            SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model));
        const bool mesh = queried && options.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED &&
                          model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
        G6SHIM_INFO("d3d12_xs: host mesh shaders %s", mesh ? "on" : "off");
        return mesh;
    }();
    return supported;
}

D3D12_CPU_DESCRIPTOR_HANDLE Host::rtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc) {
    const std::uint32_t slot = next_rtv_.fetch_add(1, std::memory_order_relaxed) % kRtvRing;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvs_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * rtv_size_;
    device_->CreateRenderTargetView(resource, desc, handle);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Host::dsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    const std::uint32_t slot = next_dsv_.fetch_add(1, std::memory_order_relaxed) % kRtvRing;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvs_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * dsv_size_;
    device_->CreateDepthStencilView(resource, desc, handle);
    return handle;
}

std::optional<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, std::uint8_t*>> Host::upload_buffer(
    std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(bytes, 1);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&buffer)))) {
        G6SHIM_WARN("d3d12_xs: no host upload buffer of %llu bytes", static_cast<unsigned long long>(bytes));
        return std::nullopt;
    }
    void* data = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &data))) return std::nullopt;
    return std::make_pair(std::move(buffer), static_cast<std::uint8_t*>(data));
}

std::optional<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, std::uint8_t*>> Host::shared_buffer(
    std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(bytes, 1);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&buffer)))) {
        G6SHIM_WARN("d3d12_xs: no host shared buffer of %llu bytes", static_cast<unsigned long long>(bytes));
        return std::nullopt;
    }
    void* data = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &data))) return std::nullopt;
    return std::make_pair(std::move(buffer), static_cast<std::uint8_t*>(data));
}

std::unique_ptr<Host::Presenter> Host::make_presenter(std::uint32_t width, std::uint32_t height,
                                                      DXGI_FORMAT format) {
    HWND hwnd = title_window();
    if (hwnd != nullptr)
        window::adopt(hwnd);
    else
        hwnd = window::create(width, height);
    if (hwnd == nullptr) {
        log_presenter_failure(PresenterFailure::kWindow, "no window");
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        log_presenter_failure(PresenterFailure::kFactory, "DXGI factory", hr);
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kSwapChainBuffers;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
    hr = factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &desc, nullptr, nullptr, &swap_chain);
    if (FAILED(hr)) {
        log_presenter_failure(PresenterFailure::kSwapChain, "CreateSwapChainForHwnd", hr);
        return nullptr;
    }
    auto made = std::make_unique<Presenter>();
    hr = swap_chain.As(&made->swap_chain);
    if (FAILED(hr)) {
        log_presenter_failure(PresenterFailure::kQuery, "swap chain query", hr);
        return nullptr;
    }
    made->copy = new_list();
    if (!made->copy) {
        log_presenter_failure(PresenterFailure::kCopy, "copy list");
        return nullptr;
    }
    made->copy->close();
    made->width = width;
    made->height = height;
    made->format = format;
    made->hwnd = hwnd;
    RECT client{};
    ::GetClientRect(hwnd, &client);
    G6SHIM_INFO("d3d12_xs: presenting %ux%u on a %ldx%ld client window %p", width, height,
                client.right - client.left, client.bottom - client.top, hwnd);
    window::show_render_size(hwnd, width, height);
    return made;
}

std::optional<bool> Host::present(ID3D12Resource* texture, std::uint32_t state, std::uint64_t frame) {
    if (texture == nullptr) return std::nullopt;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const auto format = swap_format(static_cast<int>(desc.Format));
    if (!format) {
        G6SHIM_WARN("d3d12_xs: cannot present format %d", static_cast<int>(desc.Format));
        return std::nullopt;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(desc.Width);
    const std::uint32_t height = desc.Height;
    std::lock_guard<std::mutex> lock(presenter_mutex_);
    if (!presenter_) presenter_ = make_presenter(width, height, *format);
    Presenter* presenter = presenter_.get();
    if (presenter == nullptr) {
        log_presentation_skipped();
        return std::nullopt;
    }
    if (presenter->width != width || presenter->height != height || presenter->format != *format) {
        if (FAILED(presenter->swap_chain->ResizeBuffers(kSwapChainBuffers, width, height, *format,
                                                        DXGI_SWAP_CHAIN_FLAG(0)))) {
            G6SHIM_WARN("d3d12_xs: swap chain not resized to %ux%u", width, height);
            return std::nullopt;
        }
        presenter->width = width;
        presenter->height = height;
        presenter->format = *format;
        window::show_render_size(presenter->hwnd, width, height);
    }
    const auto wanted = static_cast<D3D12_RESOURCE_STATES>(state);
    const UINT index = presenter->swap_chain->GetCurrentBackBufferIndex();
    Microsoft::WRL::ComPtr<ID3D12Resource> back;
    if (FAILED(presenter->swap_chain->GetBuffer(index, IID_PPV_ARGS(&back)))) return std::nullopt;
    presenter->copy->reset();
    ID3D12GraphicsCommandList* list = presenter->copy->list();
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{
        transition(texture, wanted, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    list->CopyResource(back.Get(), texture);
    const auto number = static_cast<unsigned>(frame);
    Capture capture;
    const bool capturing = capture_requested(number) && begin_capture(device_.Get(), list, texture, desc, capture);
    barriers[0] = transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, wanted);
    barriers[1] = transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    presenter->copy->close();
    execute({presenter->copy.get()});
    presenter->swap_chain->Present(kPresentInterval, 0);
    if (capturing) finish_capture(number, desc, capture);
    presenter->meter.frame(presenter->hwnd);
    return false;
}

void Host::log_composition_failure() {
    if (composition_failed_) return;
    composition_failed_ = true;
    G6SHIM_WARN("d3d12_xs: plane composition unavailable; showing the first plane");
}

ID3D12PipelineState* Host::pipeline_locked(DXGI_FORMAT format) {
    for (auto& cached : composer_->pipelines) {
        if (cached.format != format) continue;
        return cached.failed ? nullptr : cached.state.Get();
    }
    Composer::Pipeline* slot = nullptr;
    for (auto& cached : composer_->pipelines) {
        if (cached.format == DXGI_FORMAT_UNKNOWN) {
            slot = &cached;
            break;
        }
    }
    if (slot == nullptr) slot = &composer_->pipelines[0];
    slot->format = format;
    slot->state.Reset();
    if (!make_compose_pipeline(device_.Get(), composer_->root.Get(), format, slot->state)) {
        slot->failed = true;
        log_composition_failure();
        return nullptr;
    }
    slot->failed = false;
    return slot->state.Get();
}

bool Host::ensure_composer_locked(DXGI_FORMAT format) {
    if (composition_failed_ || !device_) return false;
    if (!composer_) composer_ = std::make_unique<Composer>();
    if (!composer_->root && !make_compose_root(device_.Get(), composer_->root)) {
        log_composition_failure();
        return false;
    }
    if (!composer_->views) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = queue::kMaxPresentedPlanes * kPresentFramesInFlight;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&composer_->views)))) {
            log_composition_failure();
            return false;
        }
        composer_->increment =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return pipeline_locked(format) != nullptr;
}

bool Host::composition_available(DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(composer_mutex_);
    return ensure_composer_locked(format);
}

bool Host::compose_planes(ID3D12GraphicsCommandList* list, std::span<const PresentedPlane> planes,
                          ID3D12Resource* target) {
    if (list == nullptr || target == nullptr || planes.empty() || planes.size() > queue::kMaxPresentedPlanes)
        return false;
    for (const PresentedPlane& plane : planes) {
        if (!sampleable(plane.texture)) return false;
    }
    if (!sampleable(target)) return false;
    const D3D12_RESOURCE_DESC target_desc = target->GetDesc();
    std::lock_guard<std::mutex> lock(composer_mutex_);
    if (!ensure_composer_locked(target_desc.Format)) return false;
    ID3D12PipelineState* pipeline = pipeline_locked(target_desc.Format);
    if (pipeline == nullptr || composer_->views == nullptr) return false;

    const std::uint32_t base = composer_->ring * queue::kMaxPresentedPlanes;
    composer_->ring = (composer_->ring + 1) % kPresentFramesInFlight;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = composer_->views->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(base) * composer_->increment;
    for (std::uint32_t slot = 0; slot < queue::kMaxPresentedPlanes; ++slot) {
        ID3D12Resource* texture = slot < planes.size() ? planes[slot].texture : planes[0].texture;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = colour_view_format(texture->GetDesc().Format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(texture, &view, cpu);
        cpu.ptr += composer_->increment;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE table = composer_->views->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(base) * composer_->increment;
    const auto width = static_cast<std::uint32_t>(target_desc.Width);
    const auto height = target_desc.Height;
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const D3D12_CPU_DESCRIPTOR_HANDLE render_target = rtv(target, nullptr);
    ID3D12DescriptorHeap* heap = composer_->views.Get();
    list->SetDescriptorHeaps(1, &heap);
    list->SetGraphicsRootSignature(composer_->root.Get());
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootDescriptorTable(0, table);
    list->SetGraphicsRoot32BitConstant(1, static_cast<UINT>(planes.size()), 0);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &render_target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return true;
}

bool compose(ID3D12GraphicsCommandList* list, std::span<const PresentedPlane> planes, ID3D12Resource* target) {
    Host* gpu = host();
    if (gpu == nullptr) return false;
    return gpu->compose_planes(list, planes, target);
}

std::optional<bool> Host::present(std::span<const PresentedPlane> planes, std::uint64_t frame) {
    if (planes.empty() || planes[0].texture == nullptr) return std::nullopt;
    const std::size_t count = std::min(planes.size(), static_cast<std::size_t>(queue::kMaxPresentedPlanes));
    const auto shown = planes.first(count);
    const D3D12_RESOURCE_DESC desc = shown[0].texture->GetDesc();
    const auto format = swap_format(static_cast<int>(desc.Format));
    const bool composable = format && count > 1 && std::all_of(shown.begin(), shown.end(), [](const PresentedPlane& plane) {
                                return sampleable(plane.texture);
                            }) &&
                            composition_available(*format);
    if (!composable) return present(shown[0].texture, shown[0].state, frame);

    const std::uint32_t width = static_cast<std::uint32_t>(desc.Width);
    const std::uint32_t height = desc.Height;
    std::lock_guard<std::mutex> lock(presenter_mutex_);
    if (!presenter_) presenter_ = make_presenter(width, height, *format);
    Presenter* presenter = presenter_.get();
    if (presenter == nullptr) return std::nullopt;
    if (presenter->width != width || presenter->height != height || presenter->format != *format) {
        if (FAILED(presenter->swap_chain->ResizeBuffers(kSwapChainBuffers, width, height, *format,
                                                        DXGI_SWAP_CHAIN_FLAG(0)))) {
            G6SHIM_WARN("d3d12_xs: swap chain not resized to %ux%u", width, height);
            return std::nullopt;
        }
        presenter->width = width;
        presenter->height = height;
        presenter->format = *format;
        window::show_render_size(presenter->hwnd, width, height);
    }
    const UINT index = presenter->swap_chain->GetCurrentBackBufferIndex();
    Microsoft::WRL::ComPtr<ID3D12Resource> back;
    if (FAILED(presenter->swap_chain->GetBuffer(index, IID_PPV_ARGS(&back)))) return std::nullopt;
    presenter->copy->reset();
    ID3D12GraphicsCommandList* list = presenter->copy->list();
    BarrierBatch barriers;
    for (const PresentedPlane& plane : shown)
        barriers.add(plane.texture, static_cast<D3D12_RESOURCE_STATES>(plane.state),
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.add(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers.record(list);
    const bool composed = compose_planes(list, shown, back.Get());
    const auto number = static_cast<unsigned>(frame);
    Capture capture;
    bool capturing = false;
    D3D12_RESOURCE_DESC capture_desc = desc;
    if (!composed) {
        for (const PresentedPlane& plane : shown)
            barriers.add(plane.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.add(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.record(list);
        list->CopyResource(back.Get(), shown[0].texture);
        capturing = capture_requested(number) && begin_capture(device_.Get(), list, shown[0].texture, desc, capture);
        for (const PresentedPlane& plane : shown)
            barriers.add(plane.texture, D3D12_RESOURCE_STATE_COPY_SOURCE,
                         static_cast<D3D12_RESOURCE_STATES>(plane.state));
        barriers.add(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        barriers.record(list);
    } else {
        D3D12_RESOURCE_STATES back_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        capture_desc = back->GetDesc();
        if (capture_requested(number)) {
            barriers.add(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers.record(list);
            back_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            capturing = begin_capture(device_.Get(), list, back.Get(), capture_desc, capture);
        }
        for (const PresentedPlane& plane : shown)
            barriers.add(plane.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         static_cast<D3D12_RESOURCE_STATES>(plane.state));
        barriers.add(back.Get(), back_state, D3D12_RESOURCE_STATE_PRESENT);
        barriers.record(list);
    }
    presenter->copy->close();
    execute({presenter->copy.get()});
    presenter->swap_chain->Present(kPresentInterval, 0);
    if (capturing) finish_capture(number, capture_desc, capture);
    presenter->meter.frame(presenter->hwnd);
    return false;
}

void name(IUnknown* object, const std::string& text) {
    if (object == nullptr) return;
    Microsoft::WRL::ComPtr<ID3D12Object> named;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&named)))) return;
    const std::wstring wide = widen(text);
    named->SetName(wide.c_str());
}

void fail(const std::string& text) {
    std::string message = text;
    char detail[128] = {};
    if (Host* gpu = host()) {
        const HRESULT reason = gpu->device()->GetDeviceRemovedReason();
        if (FAILED(reason)) {
            std::snprintf(detail, sizeof detail, "; device removed: 0x%08lX", static_cast<unsigned long>(reason));
            message += detail;
        }
        if (const auto first = gpu->log_messages()) message += "; debug layer: " + *first;
    }
    G6SHIM_ERROR("d3d12_xs: host GPU failure: %s", message.c_str());
    ::TerminateProcess(::GetCurrentProcess(), kDeviceRemoved);
    std::abort();
}

void ensure_state(HostList& list, Object* resource, std::uint32_t state) {
    const auto changed = list.transition(resource, state);
    if (!changed) return;
    const std::uint32_t wanted = legal_state(changed->first, state);
    D3D12_RESOURCE_BARRIER barrier =
        transition(changed->first, static_cast<D3D12_RESOURCE_STATES>(changed->second),
                   static_cast<D3D12_RESOURCE_STATES>(wanted));
    list.list()->ResourceBarrier(1, &barrier);
}

void copy_mapped(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes,
                 bool title_destination) {
    if (bytes == 0 || (title_destination && !texture::committed(destination, bytes))) return;
    if (title_destination) watch::writing(reinterpret_cast<std::size_t>(destination), bytes);
    if (read_process(source, destination, bytes)) return;
    for_pieces(reinterpret_cast<std::size_t>(source), bytes, kPieceBytes, [&](std::size_t at, std::size_t piece) {
        std::uint8_t* to = destination + (at - reinterpret_cast<std::size_t>(source));
        if (read_process(reinterpret_cast<const std::uint8_t*>(at), to, piece)) return;
        for_pieces(at, piece, kFinePieceBytes, [&](std::size_t fine_at, std::size_t fine) {
            std::uint8_t* fine_to = destination + (fine_at - reinterpret_cast<std::size_t>(source));
            if (!read_process(reinterpret_cast<const std::uint8_t*>(fine_at), fine_to, fine))
                std::memset(fine_to, 0, fine);
        });
    });
}

bool read_into(const std::uint8_t* source, std::span<std::uint8_t> destination) {
    return read_process(source, destination.data(), destination.size());
}

void write_mapped(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes) {
    if (bytes == 0 || write_process(destination, source, bytes)) return;
    for_pieces(reinterpret_cast<std::size_t>(destination), bytes, kPieceBytes,
               [&](std::size_t at, std::size_t piece) {
                   const std::uint8_t* from = source + (at - reinterpret_cast<std::size_t>(destination));
                   if (write_process(reinterpret_cast<std::uint8_t*>(at), from, piece)) return;
                   for_pieces(at, piece, kFinePieceBytes, [&](std::size_t fine_at, std::size_t fine) {
                       const std::uint8_t* fine_from =
                           source + (fine_at - reinterpret_cast<std::size_t>(destination));
                       write_process(reinterpret_cast<std::uint8_t*>(fine_at), fine_from, fine);
                   });
               });
}

}  // namespace host

namespace g6shim::d3d12_xs::host {

void HostList::close() {
    if (FAILED(list_->Close()))
        fail("a host command list did not close: a command recorded into it was invalid");
}

void HostList::reset() {
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        deferred_ = Deferred{};
    }
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        states_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        pipelines_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(scratch_mutex_);
        current_chunk_ = 0;
        used_ = 0;
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
        fail("a host command list did not reset");
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> HostList::transition(Object* object,
                                                                              std::uint32_t wanted) {
    ID3D12Resource* texture = texture::host_texture(object);
    std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (texture == nullptr || !copy) return std::nullopt;
    const std::uint32_t state = legal_state(texture, wanted);
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto [entry, inserted] = states_.try_emplace(reinterpret_cast<std::uintptr_t>(copy.get()));
    if (inserted) {
        entry->second = State{state, state, copy};
        return std::nullopt;
    }
    const std::uint32_t before = entry->second.last;
    entry->second.last = state;
    if (before != state) return std::make_pair(texture, before);
    return std::nullopt;
}

void HostList::defer_copy(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(CopyStep{destination, source, bytes, true});
    deferred_.refreshed.clear();
    deferred_.refreshed_sets.clear();
}

void HostList::defer_fill(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(CopyStep{destination, source, bytes, false});
}

void HostList::defer_refresh(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address,
                             std::size_t bytes) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    const auto key = std::make_tuple(reinterpret_cast<std::size_t>(shadow.get()), address, bytes);
    if (deferred_.refreshed.insert(key).second)
        deferred_.copies.push_back(RefreshStep{shadow, address, bytes});
}

void HostList::defer_gpu_write(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address,
                               std::size_t bytes) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(GpuWriteStep{shadow, address, bytes});
}

void HostList::defer_write(std::uint8_t* destination, Written written, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.writes.push_back(WriteStep{destination, written, bytes});
}

void HostList::defer(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(WorkStep{std::move(work), true});
    deferred_.refreshed.clear();
    deferred_.refreshed_sets.clear();
}

void HostList::defer_host(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(WorkStep{std::move(work), false});
}

void HostList::defer_texture(std::shared_ptr<texture::Staging> staging) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.copies.push_back(TextureStep{std::move(staging)});
}

std::optional<std::pair<std::uint64_t, std::uint8_t*>> HostList::upload(std::uint64_t bytes) {
    auto made = upload_with_resource(bytes);
    if (!made) return std::nullopt;
    return std::make_pair(made->gpu, made->data);
}

std::optional<HostList::Upload> HostList::upload_with_resource(std::uint64_t bytes) {
    Host* gpu = host();
    if (gpu == nullptr) return std::nullopt;
    auto buffer = gpu->upload_buffer(bytes);
    if (!buffer) return std::nullopt;
    name(buffer->first.Get(), "list upload");
    Upload made;
    made.gpu = buffer->first->GetGPUVirtualAddress();
    made.data = buffer->second;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        deferred_.uploads.push_back(buffer->first);
    }
    made.buffer = std::move(buffer->first);
    return made;
}

std::optional<std::pair<std::uint64_t, std::uint8_t*>> HostList::scratch(std::uint64_t bytes) {
    Host* gpu = host();
    if (gpu == nullptr) return std::nullopt;
    const std::uint64_t length = (std::max<std::uint64_t>(bytes, 1) + 255) / 256 * 256;
    if (length > kScratchChunk) {
        auto buffer = gpu->upload_buffer(length);
        if (!buffer) return std::nullopt;
        name(buffer->first.Get(), "list scratch");
        const std::uint64_t address = buffer->first->GetGPUVirtualAddress();
        {
            std::lock_guard<std::mutex> lock(deferred_mutex_);
            deferred_.uploads.push_back(buffer->first);
        }
        return std::make_pair(address, buffer->second);
    }
    std::uint64_t at = 0;
    {
        std::lock_guard<std::mutex> lock(scratch_mutex_);
        if (used_ + length > kScratchChunk && current_chunk_ < chunks_.size()) {
            ++current_chunk_;
            used_ = 0;
        }
        if (current_chunk_ == chunks_.size()) {
            auto buffer = gpu->upload_buffer(kScratchChunk);
            if (!buffer) return std::nullopt;
            name(buffer->first.Get(), "list scratch");
            ScratchChunk chunk;
            chunk.buffer = buffer->first;
            chunk.data = buffer->second;
            chunk.gpu = buffer->first->GetGPUVirtualAddress();
            chunks_.push_back(chunk);
            used_ = 0;
        }
        at = used_;
        used_ += length;
        const ScratchChunk& chunk = chunks_[current_chunk_];
        return std::make_pair(chunk.gpu + at, chunk.data + at);
    }
}

std::optional<std::uint64_t> HostList::budget() {
    std::call_once(budget_once_, [this] {
        Host* gpu = host();
        if (gpu == nullptr) return;
        auto buffer = gpu->shared_buffer(256);
        if (!buffer) return;
        budget_buffer_ = buffer->first;
        budget_data_ = buffer->second;
        budget_gpu_ = buffer->first->GetGPUVirtualAddress();
        budget_available_ = true;
    });
    if (!budget_available_) return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        if (!deferred_.budget_zeroed) {
            deferred_.budget_zeroed = true;
            auto* word = reinterpret_cast<volatile std::uint32_t*>(budget_data_);
            deferred_.copies.push_back(WorkStep{[word] { *word = 0; }, false});
        }
    }
    return budget_gpu_;
}

void HostList::keep(IUnknown* object) {
    if (object == nullptr) return;
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.kept.emplace_back(object);
}

bool HostList::first_use(std::size_t key) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    return deferred_.textures.insert(key).second;
}

bool HostList::first_refresh(const void* owner, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    return deferred_.refreshed_sets.emplace(owner, generation).second;
}

void HostList::used_pipeline(const std::string& name) {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    pipelines_.insert(name);
}

ID3D12CommandList* HostList::prelude() {
    std::lock_guard<std::mutex> lock(states_mutex_);
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (auto& [key, state] : states_) {
        const std::uint32_t real = state.copy->state.exchange(state.last, std::memory_order_relaxed);
        if (real != state.first)
            barriers.push_back(host::transition(state.copy->texture.Get(),
                                               static_cast<D3D12_RESOURCE_STATES>(real),
                                               static_cast<D3D12_RESOURCE_STATES>(state.first)));
    }
    if (barriers.empty()) return nullptr;
    if (FAILED(prelude_allocator_->Reset()) || FAILED(prelude_->Reset(prelude_allocator_.Get(), nullptr)))
        fail("a host state prelude did not record");
    prelude_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    if (FAILED(prelude_->Close())) fail("a host state prelude did not close");
    return prelude_.Get();
}

void HostList::run_deferred(Batch& batch) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    for (const Step& step : deferred_.copies) {
        if (const auto* copy = std::get_if<CopyStep>(&step)) {
            if (copy->title_memory) batch.title_written();
            copy_mapped(copy->destination, copy->source, copy->bytes, copy->title_memory);
        } else if (const auto* refresh = std::get_if<RefreshStep>(&step)) {
            batch.add(refresh->shadow, refresh->address, refresh->bytes);
        } else if (const auto* work = std::get_if<WorkStep>(&step)) {
            if (work->title_memory) batch.title_written();
            work->work();
        } else if (const auto* write = std::get_if<GpuWriteStep>(&step)) {
            batch.flush();
            write->shadow->writing(write->address, write->bytes);
        } else if (const auto* staged = std::get_if<TextureStep>(&step)) {
            if (batch.texture_due(staged->staging.get())) staged->staging->refresh();
        }
    }
}

}  // namespace g6shim::d3d12_xs::host
