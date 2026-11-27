// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.hpp"

#include "g_present_plane_ps.h"
#include "g_present_plane_vs.h"
#include "card.hpp"
#include "descriptor_host.hpp"
#include "gpu_capture.hpp"
#include "inplace.hpp"
#include "queue.hpp"
#include "removal.hpp"
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
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>

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
    removal::mark(list, "capture copy");
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

bool copy_guarded(const std::uint8_t* source, std::uint8_t* destination, std::size_t bytes) {
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (::GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

UnreadablePages& unreadable_pages() {
    static UnreadablePages pages;
    return pages;
}

bool read_process(const std::uint8_t* source, std::uint8_t* destination, std::size_t bytes) {
    const auto at = reinterpret_cast<std::size_t>(source);
    UnreadablePages& unreadable = unreadable_pages();
    if (!unreadable.known(at, bytes, watch::remaps())) {
        if (copy_guarded(source, destination, bytes)) return true;
        unreadable.note(at, bytes, watch::remaps());
    }
    SIZE_T done = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), source, destination, bytes, &done) != 0;
}

bool write_process(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes) {
    if (copy_guarded(source, destination, bytes)) return true;
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

IntervalTiming take_interval_timing();

void log_host_memory() {
    Host* gpu = host();
    if (gpu == nullptr) return;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(gpu->device()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
        return;
    const shadow::BufferStats buffers = shadow::buffer_stats();
    G6SHIM_INFO("d3d12_xs: host memory local %llu of %llu MiB, shared %llu MiB, title buffers %zu with %llu MiB "
                "(%zu large with %llu MiB), %zu placed textures, %llu retired objects",
                static_cast<unsigned long long>(local.CurrentUsage >> 20),
                static_cast<unsigned long long>(local.Budget >> 20),
                static_cast<unsigned long long>(shared.CurrentUsage >> 20), buffers.buffers,
                static_cast<unsigned long long>(buffers.bytes >> 20), buffers.large,
                static_cast<unsigned long long>(buffers.large_bytes >> 20), descriptor_host::placed_texture_count(),
                static_cast<unsigned long long>(retired_count_for_test()));
    constexpr std::size_t kGroupsShown = 8;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> groups;
    std::uint64_t total = 0;
    const std::vector<texture::GpuRange> textures = texture::live_textures();
    for (const texture::GpuRange& range : textures) {
        auto& group = groups[range.name];
        ++group.first;
        group.second += range.bytes;
        total += range.bytes;
    }
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>> ordered(groups.begin(), groups.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    std::string text;
    for (std::size_t index = 0; index < ordered.size() && index < kGroupsShown; ++index) {
        char part[160] = {};
        std::snprintf(part, sizeof part, "; %llu of [%s] %llu MiB",
                      static_cast<unsigned long long>(ordered[index].second.first), ordered[index].first.c_str(),
                      static_cast<unsigned long long>(ordered[index].second.second >> 20));
        text += part;
    }
    const resource::TextureObjects objects = resource::texture_objects();
    const descriptor_host::PlacedStats placed = descriptor_host::placed_stats();
    G6SHIM_INFO("d3d12_xs: host textures %zu with %llu MiB (title textures: %lld committed, %lld placed, %lld at "
                "an address; %llu replaced at their address, %llu slots cleared)%s",
                textures.size(), static_cast<unsigned long long>(total >> 20),
                static_cast<long long>(objects.committed), static_cast<long long>(objects.placed),
                static_cast<long long>(objects.at_address), static_cast<unsigned long long>(placed.replaced),
                static_cast<unsigned long long>(placed.slots_cleared), text.c_str());
    std::string view_base;
    std::string resident;
    for (std::size_t bucket = 0; bucket < descriptor_host::kMipBuckets; ++bucket) {
        view_base += (bucket == 0 ? "" : "/") + std::to_string(placed.view_base_mip[bucket]);
        resident += (bucket == 0 ? "" : "/") + std::to_string(placed.first_resident_mip[bucket]);
    }
    G6SHIM_INFO("d3d12_xs: placed textures by view base mip 0..5+ %s, by first mip with memory 0..5+ %s, %llu "
                "without a layout",
                view_base.c_str(), resident.c_str(), static_cast<unsigned long long>(placed.unknown_layout));
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
        const shadow::LooseStats loose = shadow::loose_stats();
        const IntervalTiming interval = take_interval_timing();
        const descriptor_host::DescriptorStats descriptors = descriptor_host::take_descriptor_stats();
        if (settings::enabled("G6SHIM_FRAME_TIMING")) {
            G6SHIM_INFO("d3d12_xs: %.1f fps, %.1f ms/frame, loose title memory %zu windows %llu MiB, %llu evicted, "
                        "descriptors marked %llu materialized %llu skipped %llu, "
                        "%llu submissions, prepare %.1f/%.1f ms %llu bytes, execute %.1f/%.1f ms, wait %.1f/%.1f ms, "
                        "copy-back %.1f/%.1f ms %llu bytes, gpu %.1f ms",
                        fps, ms, loose.windows, static_cast<unsigned long long>(loose.bytes >> 20),
                        static_cast<unsigned long long>(loose.evicted - evicted),
                        static_cast<unsigned long long>(descriptors.marked),
                        static_cast<unsigned long long>(descriptors.materialized),
                        static_cast<unsigned long long>(descriptors.skipped),
                        static_cast<unsigned long long>(interval.submissions), interval.prepare.milliseconds,
                        interval.prepare_max_ms, static_cast<unsigned long long>(interval.prepare.bytes),
                        interval.execute_call.milliseconds, interval.execute_max_ms, interval.wait.milliseconds,
                        interval.wait_max_ms, interval.copy_back.milliseconds, interval.copy_back_max_ms,
                        static_cast<unsigned long long>(interval.copy_back.bytes), interval.gpu.milliseconds);
            const shadow::CopyBackStats back = shadow::take_copy_back_stats();
            G6SHIM_INFO("d3d12_xs: copy-back merge %.1f ms over %llu ranges, %llu MiB compared, %llu KiB of title "
                        "memory read, %llu KiB written",
                        static_cast<double>(back.nanoseconds) / 1e6, static_cast<unsigned long long>(back.ranges),
                        static_cast<unsigned long long>(back.compared >> 20),
                        static_cast<unsigned long long>(back.read >> 10),
                        static_cast<unsigned long long>(back.written >> 10));
            const shadow::UploadStats uploads = shadow::take_upload_stats();
            G6SHIM_INFO("d3d12_xs: title memory uploaded %llu MiB to new copies, %llu MiB the title wrote, %llu MiB "
                        "copy-back wrote, %llu MiB unwatched",
                        static_cast<unsigned long long>(uploads.fresh >> 20),
                        static_cast<unsigned long long>(uploads.title >> 20),
                        static_cast<unsigned long long>(uploads.echo >> 20),
                        static_cast<unsigned long long>(uploads.unwatched >> 20));
            if (inplace::enabled()) {
                const inplace::Stats placed = inplace::stats();
                G6SHIM_INFO("d3d12_xs: title memory in place: %llu windows %llu MiB, %llu title buffers %llu MiB; "
                            "turned down: %llu not committed private or view memory, %llu not one stretch of a "
                            "section, %llu refused by the host, %llu over budget; %llu let go after remaps",
                            static_cast<unsigned long long>(placed.windows),
                            static_cast<unsigned long long>(placed.window_bytes >> 20),
                            static_cast<unsigned long long>(placed.buffers),
                            static_cast<unsigned long long>(placed.buffer_bytes >> 20),
                            static_cast<unsigned long long>(placed.not_one_kind),
                            static_cast<unsigned long long>(placed.not_one_stretch),
                            static_cast<unsigned long long>(placed.refused),
                            static_cast<unsigned long long>(placed.over_budget),
                            static_cast<unsigned long long>(placed.let_go));
            }
            log_host_memory();
        } else {
            G6SHIM_INFO("d3d12_xs: %.1f fps, %.1f ms/frame, loose title memory %zu windows %llu MiB, %llu evicted, "
                        "descriptors marked %llu materialized %llu skipped %llu, "
                        "%llu submissions, prepare %.1f/%.1f ms %llu bytes, execute %.1f/%.1f ms, wait %.1f/%.1f ms, "
                        "copy-back %.1f/%.1f ms %llu bytes",
                        fps, ms, loose.windows, static_cast<unsigned long long>(loose.bytes >> 20),
                        static_cast<unsigned long long>(loose.evicted - evicted),
                        static_cast<unsigned long long>(descriptors.marked),
                        static_cast<unsigned long long>(descriptors.materialized),
                        static_cast<unsigned long long>(descriptors.skipped),
                        static_cast<unsigned long long>(interval.submissions), interval.prepare.milliseconds,
                        interval.prepare_max_ms, static_cast<unsigned long long>(interval.prepare.bytes),
                        interval.execute_call.milliseconds, interval.execute_max_ms, interval.wait.milliseconds,
                        interval.wait_max_ms, interval.copy_back.milliseconds, interval.copy_back_max_ms,
                        static_cast<unsigned long long>(interval.copy_back.bytes));
        }
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

struct SubmissionHook {
    SubmissionStep step = nullptr;
    void* context = nullptr;
};

std::mutex submission_hook_mutex;
SubmissionHook submission_hook;

bool submission_hook_installed() {
    std::lock_guard<std::mutex> lock(submission_hook_mutex);
    return submission_hook.step != nullptr;
}
bool submission_confirmed(const SubmissionResult& result, HRESULT removed) {
    return SUCCEEDED(result.signal) && SUCCEEDED(result.armed) && result.wait == WAIT_OBJECT_0 &&
           SUCCEEDED(removed);
}

SubmissionResult wait_for_submission(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value,
                                     HANDLE event) {
    SubmissionResult result;
    if (queue == nullptr || fence == nullptr || event == nullptr) return result;
    result.signal = queue->Signal(fence, value);
    if (FAILED(result.signal)) return result;
    result.armed = fence->SetEventOnCompletion(value, event);
    if (FAILED(result.armed)) return result;
    result.wait = ::WaitForSingleObject(event, INFINITE);
    return result;
}

SubmissionResult complete_submission(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value,
                                     HANDLE event) {
    SubmissionStep step = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(submission_hook_mutex);
        step = submission_hook.step;
        context = submission_hook.context;
    }
    if (step != nullptr) return step(queue, fence, value, event, context);
    return wait_for_submission(queue, fence, value, event);
}

void log_submission_failure(const SubmissionResult& result, HRESULT removed) {
    G6SHIM_WARN(
        "d3d12_xs: host submission not completed (signal 0x%08lX, event 0x%08lX, wait 0x%08lX, device 0x%08lX)",
        static_cast<unsigned long>(result.signal), static_cast<unsigned long>(result.armed),
        static_cast<unsigned long>(result.wait), static_cast<unsigned long>(removed));
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

constexpr std::size_t kMaxTimingSamples = 4096;
std::mutex timing_mutex;
std::vector<SubmissionTiming> timing_samples;

void note_submission_timing(SubmissionTiming sample) {
    std::lock_guard<std::mutex> lock(timing_mutex);
    if (timing_samples.size() == kMaxTimingSamples) timing_samples.erase(timing_samples.begin());
    timing_samples.push_back(std::move(sample));
}

IntervalTiming take_interval_timing() {
    std::vector<SubmissionTiming> samples;
    {
        std::lock_guard<std::mutex> lock(timing_mutex);
        samples.swap(timing_samples);
    }
    return summarise_interval(samples);
}

void write_budget(char* out, std::size_t out_size, bool known, std::uint64_t bytes) {
    if (!known) {
        std::snprintf(out, out_size, "unavailable");
        return;
    }
    std::snprintf(out, out_size, "%llu MiB", static_cast<unsigned long long>(bytes >> 20));
}

void log_adapter(ID3D12Device* device, IDXGIAdapter* adapter, const DXGI_ADAPTER_DESC1& desc) {
    wchar_t name[128] = {};
    std::memcpy(name, desc.Description, sizeof name - sizeof(wchar_t));
    LARGE_INTEGER version{};
    const HRESULT umd = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
    char driver[64] = {};
    if (SUCCEEDED(umd)) {
        const auto packed = static_cast<std::uint64_t>(version.QuadPart);
        std::snprintf(driver, sizeof driver, "%u.%u.%u.%u", static_cast<unsigned>((packed >> 48) & 0xFFFF),
                      static_cast<unsigned>((packed >> 32) & 0xFFFF), static_cast<unsigned>((packed >> 16) & 0xFFFF),
                      static_cast<unsigned>(packed & 0xFFFF));
    } else {
        std::snprintf(driver, sizeof driver, "unavailable (0x%08lX)", static_cast<unsigned long>(umd));
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
    const bool architecture_ok =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof architecture));
    const char* uma = architecture_ok ? (architecture.UMA ? "yes" : "no") : "unknown";
    const char* coherent = architecture_ok ? (architecture.CacheCoherentUMA ? "yes" : "no") : "unknown";
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
    bool local_ok = false;
    bool non_local_ok = false;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
        local_ok = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
        non_local_ok =
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
    }
    char local_budget[64] = {};
    char local_use[64] = {};
    char non_local_budget[64] = {};
    write_budget(local_budget, sizeof local_budget, local_ok, local.Budget);
    write_budget(local_use, sizeof local_use, local_ok, local.CurrentUsage);
    write_budget(non_local_budget, sizeof non_local_budget, non_local_ok, non_local.Budget);
    G6SHIM_INFO("d3d12_xs: host adapter %ls, LUID %08lX:%08lX, vendor %04X, device %04X, driver %s, %s, UMA %s, "
                "cache-coherent UMA %s, local budget %s, local use %s, non-local budget %s",
                name, static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                static_cast<unsigned long>(desc.AdapterLuid.LowPart), desc.VendorId, desc.DeviceId, driver,
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? "software" : "hardware", uma, coherent, local_budget,
                local_use, non_local_budget);
}

void log_host_adapter(ID3D12Device* device) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        G6SHIM_WARN("d3d12_xs: host adapter unknown (no DXGI factory)");
        return;
    }
    const LUID luid = device->GetAdapterLuid();
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        adapter.Reset();
        if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.AdapterLuid.LowPart != luid.LowPart || desc.AdapterLuid.HighPart != luid.HighPart) continue;
        log_adapter(device, adapter.Get(), desc);
        return;
    }
    G6SHIM_WARN("d3d12_xs: host adapter LUID %08lX:%08lX was not enumerated",
                static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
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
    for (const auto& [shadow_entry, from, to] : refreshes_)
        copied_ += shadow_entry->update(from, to - from);
    refreshes_.clear();
}

Host::Host() {
    gpu_capture::load();
    const bool debug = settings::enabled("G6SHIM_D3D_DEBUG");
    if (debug) {
        Microsoft::WRL::ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            layer->EnableDebugLayer();
        else
            G6SHIM_WARN("d3d12_xs: no host D3D12 debug layer");
    }
    removal::enable();
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
    name(queue_.Get(), "host queue");
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
    log_host_adapter(device_.Get());
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

struct Retired {
    std::uint64_t until = 0;
    Microsoft::WRL::ComPtr<IUnknown> com;
    std::shared_ptr<void> hold;
};

std::mutex g_retired_mutex;
std::deque<Retired> g_retired;
std::atomic<std::uint64_t> g_free_faults{0};
std::atomic<std::int64_t> g_lag_override{-1};
std::once_flag g_free_fault_logged;

bool limited_host_ram() {
    static const bool limited = [] {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof status;
        if (::GlobalMemoryStatusEx(&status) == 0) return false;
        constexpr std::uint64_t kGib = 1024ull * 1024ull * 1024ull;
        return status.ullTotalPhys <= 32 * kGib;
    }();
    return limited;
}

std::uint64_t configured_bury_lag() {
    static const std::uint64_t lag = [] {
        const std::uint64_t fallback = limited_host_ram() ? 16 : 64;
        const std::optional<std::string> value = settings::get("G6SHIM_BURY_LAG");
        if (!value || value->empty()) return fallback;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value->c_str(), &end, 10);
        if (end == value->c_str()) return fallback;
        return static_cast<std::uint64_t>(parsed);
    }();
    return lag;
}

std::uint64_t finished_fence() {
    Host* gpu = host();
    if (gpu == nullptr || gpu->fence() == nullptr) return 0;
    const UINT64 value = gpu->fence()->GetCompletedValue();
    if (value == UINT64_MAX) return gpu->published_submission();
    return value;
}

void note_free_during_submission() {
    g_free_faults.fetch_add(1, std::memory_order_relaxed);
    std::call_once(g_free_fault_logged, [] {
        G6SHIM_WARN("d3d12_xs: a host object was freed while a submission was in flight");
    });
}

void enqueue_retired(Microsoft::WRL::ComPtr<IUnknown> com, std::shared_ptr<void> hold) {
    if (!com && !hold) return;
    const std::uint64_t lag = bury_lag();
    Host* gpu = host();
    const std::uint64_t started = gpu != nullptr ? gpu->published_submission() : 0;
    const std::uint64_t finished = finished_fence();
    if (started > finished) note_free_during_submission();
    if (lag == 0 && started <= finished) return;
    Retired entry;
    entry.until = started + lag;
    entry.com = std::move(com);
    entry.hold = std::move(hold);
    std::lock_guard<std::mutex> lock(g_retired_mutex);
    g_retired.push_back(std::move(entry));
}

void append_named(std::vector<Microsoft::WRL::ComPtr<IUnknown>>& objects,
                  const std::shared_ptr<later::HostTexture>& copy) {
    if (!copy) return;
    if (copy->texture) objects.emplace_back(copy->texture.Get());
    if (!copy->depth_uav) return;
    if (copy->depth_uav->depth.texture) objects.emplace_back(copy->depth_uav->depth.texture.Get());
    if (copy->depth_uav->stencil.texture) objects.emplace_back(copy->depth_uav->stencil.texture.Get());
}

void keep_named(HostList& list, const std::shared_ptr<later::HostTexture>& copy) {
    if (!copy) return;
    if (copy->texture) list.keep(copy->texture.Get());
    if (!copy->depth_uav) return;
    if (copy->depth_uav->depth.texture) list.keep(copy->depth_uav->depth.texture.Get());
    if (copy->depth_uav->stencil.texture) list.keep(copy->depth_uav->stencil.texture.Get());
}

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

Host::~Host() {
    stop_completion();
    drain_retired();
}

Host* Host::get() {
    static Host* instance = []() -> Host* {
        std::unique_ptr<Host> made(new Host());
        if (!made->device_) return nullptr;
        return made.release();
    }();
    return instance;
}

Host* host() { return Host::get(); }

std::uint64_t bury_lag() {
    const std::int64_t over = g_lag_override.load(std::memory_order_relaxed);
    if (over >= 0) return static_cast<std::uint64_t>(over);
    return configured_bury_lag();
}

void retire(Microsoft::WRL::ComPtr<IUnknown> object) { enqueue_retired(std::move(object), nullptr); }

void retire_hold(std::shared_ptr<void> hold) { enqueue_retired(nullptr, std::move(hold)); }

void release_retired() {
    Host* gpu = host();
    if (gpu != nullptr && gpu->fence() != nullptr && gpu->fence()->GetCompletedValue() == UINT64_MAX) {
        drain_retired();
        return;
    }
    const std::uint64_t finished = finished_fence();
    std::vector<Retired> done;
    {
        std::lock_guard<std::mutex> lock(g_retired_mutex);
        std::deque<Retired> waiting;
        for (Retired& entry : g_retired) {
            if (entry.until <= finished) done.push_back(std::move(entry));
            else waiting.push_back(std::move(entry));
        }
        g_retired.swap(waiting);
    }
}

void drain_retired() {
    std::deque<Retired> dropped;
    {
        std::lock_guard<std::mutex> lock(g_retired_mutex);
        dropped.swap(g_retired);
    }
}

std::uint32_t references(IUnknown* object) {
    if (object == nullptr) return 0;
    object->AddRef();
    return object->Release();
}

bool submission_in_flight() {
    Host* gpu = host();
    if (gpu == nullptr || gpu->fence() == nullptr) return false;
    if (gpu->fence()->GetCompletedValue() == UINT64_MAX) return false;
    return gpu->published_submission() > gpu->fence()->GetCompletedValue();
}

std::uint64_t retired_count_for_test() {
    std::lock_guard<std::mutex> lock(g_retired_mutex);
    return g_retired.size();
}

std::uint64_t free_during_submission_faults_for_test() {
    return g_free_faults.load(std::memory_order_relaxed);
}

void set_bury_lag_for_test(std::uint64_t lag) {
    g_lag_override.store(static_cast<std::int64_t>(lag), std::memory_order_relaxed);
}

void reset_bury_lag_for_test() { g_lag_override.store(-1, std::memory_order_relaxed); }

void set_submission_step_for_test(SubmissionStep step, void* context) {
    std::lock_guard<std::mutex> lock(submission_hook_mutex);
    submission_hook.step = step;
    submission_hook.context = context;
}

void add_submission_timing(IntervalTiming& interval, const SubmissionTiming& sample) {
    ++interval.submissions;
    interval.prepare.milliseconds += sample.prepare.milliseconds;
    interval.prepare.bytes += sample.prepare.bytes;
    interval.prepare_max_ms = std::max(interval.prepare_max_ms, sample.prepare.milliseconds);
    interval.execute_call.milliseconds += sample.execute_call.milliseconds;
    interval.execute_max_ms = std::max(interval.execute_max_ms, sample.execute_call.milliseconds);
    interval.wait.milliseconds += sample.wait.milliseconds;
    interval.wait_max_ms = std::max(interval.wait_max_ms, sample.wait.milliseconds);
    interval.copy_back.milliseconds += sample.copy_back.milliseconds;
    interval.copy_back.bytes += sample.copy_back.bytes;
    interval.copy_back_max_ms = std::max(interval.copy_back_max_ms, sample.copy_back.milliseconds);
    if (!sample.gpu_known) return;
    interval.gpu.milliseconds += sample.gpu.milliseconds;
    interval.gpu.bytes += sample.gpu.bytes;
    ++interval.gpu_samples;
}

IntervalTiming summarise_interval(std::span<const SubmissionTiming> samples) {
    IntervalTiming interval;
    for (const SubmissionTiming& sample : samples) add_submission_timing(interval, sample);
    return interval;
}

std::optional<double> last_gpu_milliseconds_for_test() {
    std::lock_guard<std::mutex> lock(timing_mutex);
    for (auto sample = timing_samples.rbegin(); sample != timing_samples.rend(); ++sample)
        if (sample->gpu_known) return sample->gpu.milliseconds;
    return std::nullopt;
}

std::string name_command_list(ID3D12GraphicsCommandList* list) {
    static std::atomic<std::uint64_t> next{0};
    const auto number = next.fetch_add(1, std::memory_order_relaxed) + 1;
    char text[32] = {};
    std::snprintf(text, sizeof text, "host list %llu", static_cast<unsigned long long>(number));
    removal::name_list(list, text);
    return text;
}

void mark_work(HostList& list, std::string_view label) {
    removal::mark(list.list(), label);
    list.note_marker(std::string(label));
}

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
    made->list_name(name_command_list(made->list_.Get()));
    name_command_list(made->prelude_.Get());
    return made;
}

void record_submission_timing(SubmissionTiming sample) { note_submission_timing(std::move(sample)); }

IntervalTiming take_submission_interval() { return take_interval_timing(); }

void Host::execute(const std::vector<HostList*>& runs) {
    const bool async =
        submits_asynchronously(completion_thread_enabled(), submission_hook_installed(), inplace::enabled());
    if (!async) wait_idle();
    Object* bound = submitting_queue();
    begin_queue_submission(nullptr);
    std::lock_guard<std::mutex> submitted_lock(submitted_mutex_);
    release_retained();
    SubmissionTiming timing;
    const auto prepared = std::chrono::steady_clock::now();
    descriptor_host::sweep_before_submission(runs);
    Batch batch;
    std::vector<ID3D12CommandList*> lists;
    lists.reserve(runs.size() * 2 + 3);
    for (HostList* list : runs) {
        if (list == nullptr) continue;
        list->run_deferred(batch);
        if (ID3D12CommandList* prelude = list->prelude()) lists.push_back(prelude);
        lists.push_back(list->list());
    }
    batch.flush();
    for (HostList* list : runs) {
        if (list == nullptr) continue;
        std::lock_guard<std::mutex> lock(list->deferred_mutex_);
        for (const Step& step : list->deferred_.copies) {
            const auto* write = std::get_if<GpuWriteStep>(&step);
            if (write == nullptr || !write->shadow) continue;
            write->shadow->arm_card(write->address, write->bytes);
        }
    }
    card::Batch cards = card::take_batch();
    const bool card_prepare = cards.needs_prepare();
    const bool card_read = cards.needs_readback();
    bool cards_open = false;
    if (card_prepare || card_read) {
        card::wait_recorder();
        if (card_prepare) {
            if (ID3D12CommandList* prepare = cards.prepare(device_.Get())) lists.insert(lists.begin(), prepare);
        }
        card::mark_recorder_busy();
        cards_open = true;
    }
    timing.prepare.milliseconds = milliseconds_since(prepared);
    timing.prepare.bytes = batch.copied();
    const bool timed = !async && bracket_timestamps(lists);
    ++submitted_;
    submitted_published_.store(submitted_, std::memory_order_release);
    const auto executed = std::chrono::steady_clock::now();
    if (!lists.empty()) queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    timing.execute_call.milliseconds = milliseconds_since(executed);
    if (!async) {
        const auto waited = std::chrono::steady_clock::now();
        const SubmissionResult result = complete_submission(queue_.Get(), fence_.Get(), submitted_, event_);
        timing.wait.milliseconds = milliseconds_since(waited);
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (!submission_confirmed(result, removed)) {
            log_submission_failure(result, removed);
            if (FAILED(removed)) {
                removal::report(device_.Get());
                for (HostList* list : runs)
                    if (list != nullptr) list->discard_unpublished();
                held_.clear();
            } else {
                retain_incomplete(runs, SUCCEEDED(result.signal));
            }
            log_messages();
            note_submission_timing(std::move(timing));
            if (cards_open) card::release_recorder();
            release_retired();
            return;
        }
        if (timed) {
            if (const std::optional<double> gpu = resolve_timestamps()) {
                timing.gpu.milliseconds = *gpu;
                timing.gpu_known = true;
            }
        }
        if (cards_open) cards.read_changed(device_.Get(), queue_.Get());
        const auto copied_at = std::chrono::steady_clock::now();
        std::vector<shadow::WriteRange> back;
        for (HostList* list : runs) {
            if (list == nullptr) continue;
            std::lock_guard<std::mutex> lock(list->deferred_mutex_);
            for (const Step& step : list->deferred_.copies) {
                const auto* write = std::get_if<GpuWriteStep>(&step);
                if (write != nullptr && write->shadow) back.emplace_back(write->shadow, write->address, write->bytes);
            }
        }
        shadow::CopyBackStats back_stats;
        shadow::copy_back_all(back, &back_stats);
        shadow::note_copy_back_stats(back_stats);
        const std::uint64_t copied = back_stats.written;
        if (cards_open) {
            cards.sync_kept(device_.Get(), queue_.Get());
            card::release_recorder();
            cards_open = false;
        }
        timing.copy_back.bytes = copied;
        timing.copy_back.milliseconds = milliseconds_since(copied_at);
        note_submission_timing(std::move(timing));
        for (HostList* list : runs) {
            if (list == nullptr) continue;
            std::lock_guard<std::mutex> lock(list->deferred_mutex_);
            for (const WriteStep& write : list->deferred_.writes)
                write.written.write(write.destination, write.bytes);
        }
        confirmed_ = submitted_;
        release_retained();
        log_messages();
        shadow::submission_completed();
        descriptor_host::release_retired_views();
        release_retired();
        return;
    }
    const HRESULT signalled = queue_->Signal(fence_.Get(), submitted_);
    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (FAILED(signalled) || FAILED(removed)) {
        SubmissionResult result;
        result.signal = signalled;
        result.wait = WAIT_FAILED;
        log_submission_failure(result, removed);
        if (FAILED(removed)) {
            removal::report(device_.Get());
            for (HostList* list : runs)
                if (list != nullptr) list->discard_unpublished();
            held_.clear();
        } else {
            retain_incomplete(runs, SUCCEEDED(signalled));
        }
        log_messages();
        note_submission_timing(std::move(timing));
        if (cards_open) {
            if (FAILED(signalled)) queue_->Signal(fence_.Get(), submitted_);
            complete_submission(queue_.Get(), fence_.Get(), submitted_, event_);
            card::release_recorder();
        }
        release_retired();
        return;
    }
    FlightInput flight;
    flight.host_value = submitted_;
    flight.queue = bound;
    flight.timing = timing;
    for (HostList* list : runs) {
        if (list == nullptr) continue;
        list->retain_objects(flight.kept);
        {
            std::lock_guard<std::mutex> lock(list->deferred_mutex_);
            for (const Step& step : list->deferred_.copies) {
                const auto* write = std::get_if<GpuWriteStep>(&step);
                if (write != nullptr && write->shadow)
                    flight.writes.emplace_back(write->shadow, write->address, write->bytes);
            }
            list->deferred_.copies.erase(std::remove_if(list->deferred_.copies.begin(), list->deferred_.copies.end(),
                                                        [](const Step& step) {
                                                            return std::holds_alternative<GpuWriteStep>(step);
                                                        }),
                                         list->deferred_.copies.end());
            flight.deferred.insert(flight.deferred.end(), list->deferred_.writes.begin(), list->deferred_.writes.end());
            list->deferred_.writes.clear();
        }
        {
            std::lock_guard<std::mutex> lock(list->scratch_mutex_);
            for (const auto& chunk : list->chunks_)
                if (chunk.buffer) flight.kept.emplace_back(chunk.buffer.Get());
        }
    }
    flight.cards = std::move(cards);
    flight.cards_open = cards_open;
    enqueue_flight(std::move(flight));
    release_retired();
    log_messages();
}

bool Host::ensure_timestamps() {
    if (!timestamps_) timestamps_ = std::make_unique<TimestampQueries>();
    TimestampQueries& queries = *timestamps_;
    if (queries.ready) return true;
    if (queries.failed) return false;
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = 2;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback{};
    readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback.Width = sizeof(std::uint64_t) * 2;
    readback.Height = 1;
    readback.DepthOrArraySize = 1;
    readback.MipLevels = 1;
    readback.SampleDesc.Count = 1;
    readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (FAILED(device_->CreateQueryHeap(&heap, IID_PPV_ARGS(&queries.heap))) ||
        FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&queries.readback))) ||
        FAILED(queries.readback->Map(0, nullptr, &mapped)) ||
        FAILED(queue_->GetTimestampFrequency(&queries.frequency)) || queries.frequency == 0 ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&queries.begin_allocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, queries.begin_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&queries.begin_list))) ||
        FAILED(queries.begin_list->Close()) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&queries.end_allocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, queries.end_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&queries.end_list))) ||
        FAILED(queries.end_list->Close())) {
        queries.failed = true;
        G6SHIM_WARN("d3d12_xs: host GPU timestamps unavailable");
        return false;
    }
    queries.mapped = static_cast<std::uint64_t*>(mapped);
    removal::name_list(queries.begin_list.Get(), "timestamp begin");
    removal::name_list(queries.end_list.Get(), "timestamp end");
    queries.ready = true;
    return true;
}

bool Host::bracket_timestamps(std::vector<ID3D12CommandList*>& lists) {
    if (!settings::enabled("G6SHIM_FRAME_TIMING") || !ensure_timestamps()) return false;
    TimestampQueries& queries = *timestamps_;
    if (FAILED(queries.begin_allocator->Reset()) ||
        FAILED(queries.begin_list->Reset(queries.begin_allocator.Get(), nullptr)))
        return false;
    removal::forget_markers("timestamp begin");
    removal::mark(queries.begin_list.Get(), "timestamp begin");
    queries.begin_list->EndQuery(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    if (FAILED(queries.begin_list->Close())) return false;
    if (FAILED(queries.end_allocator->Reset()) ||
        FAILED(queries.end_list->Reset(queries.end_allocator.Get(), nullptr)))
        return false;
    removal::forget_markers("timestamp end");
    removal::mark(queries.end_list.Get(), "timestamp resolve");
    queries.end_list->EndQuery(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    queries.end_list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queries.readback.Get(),
                                       0);
    if (FAILED(queries.end_list->Close())) return false;
    lists.insert(lists.begin(), queries.begin_list.Get());
    lists.push_back(queries.end_list.Get());
    return true;
}

std::optional<double> Host::resolve_timestamps() {
    if (!timestamps_ || !timestamps_->ready || timestamps_->mapped == nullptr || timestamps_->frequency == 0)
        return std::nullopt;
    const std::uint64_t begin = timestamps_->mapped[0];
    const std::uint64_t end = timestamps_->mapped[1];
    if (end < begin) return 0.0;
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(timestamps_->frequency);
}

void Host::release_retained() {
    release_retired();
    if (!device_ || FAILED(device_->GetDeviceRemovedReason())) {
        held_.clear();
        return;
    }
    const std::uint64_t done = fence_ ? fence_->GetCompletedValue() : 0;
    held_.erase(std::remove_if(held_.begin(), held_.end(),
                               [&](const HeldSubmission& held) {
                                   if (held.signalled) return done >= held.enqueued;
                                   return confirmed_ != 0 && confirmed_ >= held.enqueued;
                               }),
                held_.end());
}

void Host::retain_incomplete(const std::vector<HostList*>& runs, bool signalled) {
    HeldSubmission held;
    held.enqueued = submitted_;
    held.signalled = signalled;
    for (HostList* list : runs) {
        if (list == nullptr) continue;
        list->retain_objects(held.objects);
        list->discard_unpublished();
    }
    if (!held.objects.empty()) held_.push_back(std::move(held));
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
    removal::mark(list, "present copy");
    list->CopyResource(back.Get(), texture);
    const auto number = static_cast<unsigned>(frame);
    Capture capture;
    const bool capturing = capture_requested(number) && begin_capture(device_.Get(), list, texture, desc, capture);
    barriers[0] = transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, wanted);
    barriers[1] = transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    presenter->copy->close();
    execute({presenter->copy.get()});
    wait_idle();
    presenter->swap_chain->Present(kPresentInterval, 0);
    gpu_capture::frame_presented();
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
        descriptor_host::note_shader_heap(composer_->views->GetGPUDescriptorHandleForHeapStart().ptr,
                                          static_cast<std::uint64_t>(heap.NumDescriptors) * composer_->increment,
                                          composer_->increment, false);
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
    removal::mark(list, "present compose");
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
        removal::mark(list, "present copy");
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
    wait_idle();
    presenter->swap_chain->Present(kPresentInterval, 0);
    gpu_capture::frame_presented();
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
            removal::report(gpu->device());
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

bool UnreadablePages::known(std::size_t at, std::size_t bytes, std::uint64_t remaps) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (remaps != remaps_) return false;
    const std::size_t end = at + bytes;
    for (std::size_t page = at / kUnreadablePageBytes; page * kUnreadablePageBytes < end; ++page)
        if (pages_.count(page) != 0) return true;
    return false;
}

void UnreadablePages::note(std::size_t at, std::size_t bytes, std::uint64_t remaps) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (remaps != remaps_) {
        pages_.clear();
        remaps_ = remaps;
    }
    if (pages_.size() >= kMaxUnreadablePages) pages_.clear();
    const std::size_t end = at + bytes;
    for (std::size_t page = at / kUnreadablePageBytes; page * kUnreadablePageBytes < end; ++page) {
        MEMORY_BASIC_INFORMATION info{};
        const bool readable = ::VirtualQuery(reinterpret_cast<const void*>(page * kUnreadablePageBytes), &info,
                                             sizeof info) != 0 &&
                              info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        if (!readable) pages_.insert(page);
    }
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

StandIn stand_in() {
    static std::once_flag once;
    static Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    static StandIn made{};
    std::call_once(once, [] {
        Host* gpu = host();
        if (gpu == nullptr || gpu->device() == nullptr) return;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kStandInBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(gpu->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&buffer)))) {
            G6SHIM_WARN("d3d12_xs: no zero stand-in for an unbound root view");
            return;
        }
        void* data = nullptr;
        if (FAILED(buffer->Map(0, nullptr, &data)) || data == nullptr) {
            buffer.Reset();
            return;
        }
        std::memset(data, 0, kStandInBytes);
        buffer->Unmap(0, nullptr);
        made.resource = buffer.Get();
        made.gpu = buffer->GetGPUVirtualAddress();
    });
    return made;
}

}  // namespace host

namespace g6shim::d3d12_xs::host {

void HostList::close() {
    if (FAILED(list_->Close()))
        fail("a host command list did not close: a command recorded into it was invalid");
}

void HostList::note_marker(std::string text) {
    if (!removal::requested()) return;
    if (text.size() > removal::kMaxMarkerChars) text.resize(removal::kMaxMarkerChars);
    markers_.push_back(std::move(text));
}

void HostList::reset() {
    texture::drop_depth_uav_notes(this);
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        for (auto& upload : deferred_.uploads)
            if (upload) retire(Microsoft::WRL::ComPtr<IUnknown>(upload.Get()));
        for (auto& kept : deferred_.kept)
            if (kept) retire(Microsoft::WRL::ComPtr<IUnknown>(kept.Get()));
        deferred_ = Deferred{};
    }
    const std::string retired = list_name_;
    if (allocator_busy_) replace_busy_allocators();
    removal::forget_markers(retired);
    markers_.clear();
    {
        states_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        pipelines_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(descriptor_heaps_mutex_);
        descriptor_heaps_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(scratch_mutex_);
        for (ScratchChunk& chunk : chunks_)
            if (chunk.buffer) retire(Microsoft::WRL::ComPtr<IUnknown>(chunk.buffer.Get()));
        chunks_.clear();
        current_chunk_ = 0;
        used_ = 0;
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
        fail("a host command list did not reset");
}

void HostList::replace_busy_allocators() {
    allocator_.Reset();
    list_.Reset();
    prelude_allocator_.Reset();
    prelude_.Reset();
    allocator_busy_ = false;
    Host* gpu = host();
    if (gpu == nullptr || gpu->device() == nullptr ||
        FAILED(gpu->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
        FAILED(gpu->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                                 IID_PPV_ARGS(&list_))) ||
        FAILED(gpu->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&prelude_allocator_))) ||
        FAILED(gpu->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, prelude_allocator_.Get(),
                                                 nullptr, IID_PPV_ARGS(&prelude_))) ||
        FAILED(list_->Close()) || FAILED(prelude_->Close()))
        fail("a host command list did not replace a busy allocator");
    list_name(name_command_list(list_.Get()));
    name_command_list(prelude_.Get());
}

void HostList::discard_unpublished() {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    for (const Step& step : deferred_.copies) {
        const auto* write = std::get_if<GpuWriteStep>(&step);
        if (write != nullptr && write->shadow) write->shadow->discard_write(write->address, write->bytes);
    }
    deferred_.copies.erase(std::remove_if(deferred_.copies.begin(), deferred_.copies.end(),
                                         [](const Step& step) { return std::holds_alternative<GpuWriteStep>(step); }),
                           deferred_.copies.end());
    deferred_.writes.clear();
}

void HostList::retain_objects(std::vector<Microsoft::WRL::ComPtr<IUnknown>>& objects) {
    if (allocator_) objects.emplace_back(allocator_.Get());
    if (list_) objects.emplace_back(list_.Get());
    if (prelude_allocator_) objects.emplace_back(prelude_allocator_.Get());
    if (prelude_) objects.emplace_back(prelude_.Get());
    if (budget_buffer_) objects.emplace_back(budget_buffer_.Get());
    {
        std::lock_guard<std::mutex> states(states_mutex_);
        for (const auto& entry : states_) append_named(objects, entry.second.copy);
    }
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    for (const auto& upload : deferred_.uploads)
        if (upload) objects.emplace_back(upload.Get());
    for (const auto& kept : deferred_.kept)
        if (kept) objects.emplace_back(kept.Get());
    for (const Step& step : deferred_.copies) {
        if (const auto* staged = std::get_if<TextureStep>(&step)) {
            if (staged->staging && staged->staging->buffer()) objects.emplace_back(staged->staging->buffer());
        } else if (const auto* refresh = std::get_if<RefreshStep>(&step)) {
            if (refresh->shadow && refresh->shadow->buffer()) objects.emplace_back(refresh->shadow->buffer());
        }
    }
    allocator_busy_ = true;
}

std::optional<std::pair<ID3D12Resource*, std::uint32_t>> HostList::transition(Object* object,
                                                                              std::uint32_t wanted) {
    ID3D12Resource* texture = texture::host_texture(object);
    std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (texture == nullptr || !copy) return std::nullopt;
    keep_named(*this, copy);
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

void HostList::note_state(Object* object, std::uint32_t state) {
    std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (!copy) return;
    keep_named(*this, copy);
    std::lock_guard<std::mutex> lock(states_mutex_);
    states_[reinterpret_cast<std::uintptr_t>(copy.get())] = State{state, state, std::move(copy)};
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

void HostList::note_descriptor_heap(Object* heap) {
    if (heap == nullptr) return;
    std::lock_guard<std::mutex> lock(descriptor_heaps_mutex_);
    if (std::find(descriptor_heaps_.begin(), descriptor_heaps_.end(), heap) == descriptor_heaps_.end())
        descriptor_heaps_.push_back(heap);
}

std::vector<Object*> HostList::descriptor_heaps() const {
    std::lock_guard<std::mutex> lock(descriptor_heaps_mutex_);
    return descriptor_heaps_;
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
    constexpr double kSlowSeconds = 1.0;
    const auto began = std::chrono::steady_clock::now();
    std::uint64_t refreshes = 0;
    std::uint64_t gpu_writes = 0;
    std::uint64_t snapshot_bytes = 0;
    std::set<std::tuple<const shadow::Shadow*, std::size_t, std::size_t>> declared;
    for (const Step& step : deferred_.copies) {
        if (const auto* copy = std::get_if<CopyStep>(&step)) {
            if (copy->title_memory) batch.title_written();
            copy_mapped(copy->destination, copy->source, copy->bytes, copy->title_memory);
        } else if (const auto* refresh = std::get_if<RefreshStep>(&step)) {
            ++refreshes;
            batch.add(refresh->shadow, refresh->address, refresh->bytes);
        } else if (const auto* work = std::get_if<WorkStep>(&step)) {
            if (work->title_memory) batch.title_written();
            work->work();
        } else if (const auto* write = std::get_if<GpuWriteStep>(&step)) {
            ++gpu_writes;
            if (!declared.emplace(write->shadow.get(), write->address, write->bytes).second) continue;
            batch.flush();
            snapshot_bytes += write->shadow->writing(write->address, write->bytes);
        } else if (const auto* staged = std::get_if<TextureStep>(&step)) {
            if (batch.texture_due(staged->staging.get())) staged->staging->refresh();
        }
    }
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - began;
    if (took.count() >= kSlowSeconds)
        G6SHIM_WARN("d3d12_xs: a list's deferred work took %.1f s (%zu steps, %llu refreshes, %llu GPU writes, "
                    "%llu MiB of write snapshots)",
                    took.count(), deferred_.copies.size(), static_cast<unsigned long long>(refreshes),
                    static_cast<unsigned long long>(gpu_writes),
                    static_cast<unsigned long long>(snapshot_bytes >> 20));
}

}  // namespace g6shim::d3d12_xs::host
