// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.hpp"

#include "resource.hpp"
#include "shadow.hpp"
#include "texture.hpp"
#include "watch.hpp"
#include "window.hpp"

#include <g6shim/log.hpp>
#include <g6shim/window.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>

namespace g6shim::d3d12_xs::host {

namespace {

constexpr std::uint32_t kSwapChainBuffers = 2;
constexpr std::uint32_t kPresentInterval = 0;
constexpr std::size_t kPieceBytes = 64 << 10;
constexpr std::size_t kFinePieceBytes = 4 << 10;
constexpr DWORD kDeviceRemoved = 0x887A0005u;

bool environment_set(const char* name) {
    char value[8] = {};
    const DWORD length = ::GetEnvironmentVariableA(name, value, sizeof value);
    return length > 0 && length < sizeof value && value[0] != '0';
}

std::optional<DXGI_FORMAT> swap_format(int format) {
    if (format >= 27 && format <= 32) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == 87 || format == 90 || format == 91) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format >= 23 && format <= 25) return DXGI_FORMAT_R10G10B10A2_UNORM;
    if (format >= 9 && format <= 14) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return std::nullopt;
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
    std::chrono::steady_clock::time_point logged = std::chrono::steady_clock::now();
    std::optional<std::wstring> caption;

    void frame(HWND hwnd);
};

void set_caption(HWND hwnd, std::wstring text) {
    static std::mutex mutex;
    static std::optional<std::pair<HWND, std::wstring>> pending;
    static std::once_flag once;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::make_pair(hwnd, std::move(text));
    }
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::optional<std::pair<HWND, std::wstring>> next;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    next = std::move(pending);
                    pending.reset();
                }
                if (next)
                    ::SetWindowTextW(next->first, next->second.c_str());
                else
                    ::Sleep(100);
            }
        }).detach();
    });
}

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
        G6SHIM_INFO("d3d12_xs: %.1f fps, %.1f ms/frame", fps, ms);
    }
    if (!caption) {
        wchar_t buffer[512] = {};
        const int length = ::InternalGetWindowText(hwnd, buffer, 512);
        caption = std::wstring(buffer, static_cast<std::size_t>(length > 0 ? length : 0));
    }
    wchar_t suffix[96] = {};
    std::swprintf(suffix, 96, L" | g6shim %.0f fps %.1f ms/frame", fps, ms);
    set_caption(hwnd, *caption + suffix);
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
    const bool debug = environment_set("G6SHIM_D3D_DEBUG");
    if (debug) {
        Microsoft::WRL::ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            layer->EnableDebugLayer();
        else
            G6SHIM_WARN("d3d12_xs: no host D3D12 debug layer");
    }
    HRESULT made = E_FAIL;
    if (environment_set("G6SHIM_D3D_WARP")) {
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
    if (SUCCEEDED(signalled) && SUCCEEDED(fence_->SetEventOnCompletion(submitted_, event_))) {
        ::WaitForSingleObject(event_, INFINITE);
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
    if (hwnd == nullptr) return nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
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
    if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &desc, nullptr, nullptr, &swap_chain)))
        return nullptr;
    auto made = std::make_unique<Presenter>();
    if (FAILED(swap_chain.As(&made->swap_chain))) return nullptr;
    made->copy = new_list();
    if (!made->copy) return nullptr;
    made->copy->close();
    made->width = width;
    made->height = height;
    made->format = format;
    made->hwnd = hwnd;
    RECT client{};
    ::GetClientRect(hwnd, &client);
    G6SHIM_INFO("d3d12_xs: presenting %ux%u on a %ldx%ld client window %p", width, height,
                client.right - client.left, client.bottom - client.top, hwnd);
    return made;
}

std::optional<bool> Host::present(ID3D12Resource* texture, std::uint32_t state, std::uint64_t frame) {
    (void)frame;
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
    barriers[0] = transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, wanted);
    barriers[1] = transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    presenter->copy->close();
    execute({presenter->copy.get()});
    presenter->swap_chain->Present(kPresentInterval, 0);
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
