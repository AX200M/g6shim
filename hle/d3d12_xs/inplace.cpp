// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "inplace.hpp"

#include "host.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <atomic>
#include <charconv>
#include <mutex>
#include <set>
#include <string>

namespace g6shim::d3d12_xs::inplace {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kUnit = 64 << 10;
constexpr std::uint64_t kDefaultMostMiB = 4096;
constexpr std::uint64_t kLoggedInFull = 16;

std::atomic<FindStretch> g_finder{nullptr};
std::atomic<std::uint64_t> g_windows{0};
std::atomic<std::uint64_t> g_window_bytes{0};
std::atomic<std::uint64_t> g_buffers{0};
std::atomic<std::uint64_t> g_buffer_bytes{0};
std::atomic<std::uint64_t> g_not_one_kind{0};
std::atomic<std::uint64_t> g_not_one_stretch{0};
std::atomic<std::uint64_t> g_refused{0};
std::atomic<std::uint64_t> g_over_budget{0};
std::atomic<std::uint64_t> g_let_go{0};

std::mutex g_changed_mutex;
std::vector<std::pair<std::uint64_t, std::uint64_t>> g_changed;
std::mutex g_refused_mutex;
std::set<std::pair<std::uint64_t, std::uint64_t>> g_refused_ranges;

bool refused_before(std::uint64_t at, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(g_refused_mutex);
    return g_refused_ranges.contains({at, size});
}

void remember_refused(std::uint64_t at, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(g_refused_mutex);
    g_refused_ranges.insert({at, size});
}

std::uint64_t most_bytes() {
    static const std::uint64_t most = [] {
        const std::string text = settings::get("G6SHIM_INPLACE_MB").value_or("");
        std::uint64_t value = kDefaultMostMiB;
        if (!text.empty()) std::from_chars(text.data(), text.data() + text.size(), value);
        return value << 20;
    }();
    return most;
}

bool worth_logging(std::atomic<std::uint64_t>& count) {
    const std::uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    return n <= kLoggedInFull || (n & (n - 1)) == 0;
}

enum class Kind : std::uint8_t { kNone, kPrivate, kMapped, kMixed };

Kind kind_of(std::uint64_t at, std::uint64_t bytes) {
    Kind kind = Kind::kNone;
    std::uint64_t here = at;
    const std::uint64_t end = at + bytes;
    DWORD protection = 0;
    while (here < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<const void*>(here), &info, sizeof info) == 0) return Kind::kMixed;
        if (info.State != MEM_COMMIT) return Kind::kMixed;
        const Kind this_kind = info.Type == MEM_PRIVATE ? Kind::kPrivate
                               : info.Type == MEM_MAPPED ? Kind::kMapped
                                                         : Kind::kMixed;
        if (this_kind == Kind::kMixed) return Kind::kMixed;
        if (kind == Kind::kNone) kind = this_kind;
        if (kind != this_kind) return Kind::kMixed;
        if (kind == Kind::kPrivate) {
            if (protection == 0) protection = info.Protect;
            if (info.Protect != protection || (info.Protect & PAGE_GUARD) != 0) return Kind::kMixed;
        }
        here = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return kind;
}

bool committed(std::uint64_t at, std::uint64_t bytes) {
    std::uint64_t here = at;
    while (here < at + bytes) {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<const void*>(here), &info, sizeof info) == 0 || info.State != MEM_COMMIT)
            return false;
        here = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

ComPtr<ID3D12Heap> open_heap(ID3D12Device* device, std::uint64_t at, std::uint64_t bytes, HRESULT* result) {
    ComPtr<ID3D12Device13> device13;
    *result = device->QueryInterface(IID_PPV_ARGS(&device13));
    if (FAILED(*result)) return nullptr;
    ComPtr<ID3D12Heap> heap;
    *result = device13->OpenExistingHeapFromAddress1(reinterpret_cast<const void*>(at), static_cast<SIZE_T>(bytes),
                                                      IID_PPV_ARGS(&heap));
    return SUCCEEDED(*result) ? heap : nullptr;
}

ComPtr<ID3D12Resource> place_buffer(ID3D12Device* device, ID3D12Heap* heap, std::uint64_t bytes, HRESULT* result) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    *result =
        device->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer));
    return SUCCEEDED(*result) ? buffer : nullptr;
}

std::optional<std::uint64_t> own_view(std::uint64_t at, std::uint64_t bytes) {
    const FindStretch finder = g_finder.load(std::memory_order_acquire);
    if (finder == nullptr) return std::nullopt;
    HANDLE section = nullptr;
    std::uint64_t offset = 0;
    if (!finder(static_cast<std::uintptr_t>(at), static_cast<std::size_t>(bytes), &section, &offset) ||
        offset % kUnit != 0)
        return std::nullopt;
    void* view = ::MapViewOfFile(section, FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32),
                                 static_cast<DWORD>(offset), static_cast<SIZE_T>(bytes));
    if (view == nullptr) return std::nullopt;
    if (!committed(reinterpret_cast<std::uint64_t>(view), bytes)) {
        ::UnmapViewOfFile(view);
        return std::nullopt;
    }
    return reinterpret_cast<std::uint64_t>(view);
}

}  // namespace

Region::Region(ComPtr<ID3D12Heap> heap, ComPtr<ID3D12Resource> buffer, std::uint64_t base, std::uint64_t bytes,
               Backing backing, bool window)
    : heap_(std::move(heap)), buffer_(std::move(buffer)), base_(base), bytes_(bytes), backing_(backing),
      window_(window) {
    gpu_ = buffer_->GetGPUVirtualAddress();
    (window_ ? g_windows : g_buffers).fetch_add(1, std::memory_order_relaxed);
    (window_ ? g_window_bytes : g_buffer_bytes).fetch_add(bytes_, std::memory_order_relaxed);
}

Region::~Region() {
    (window_ ? g_windows : g_buffers).fetch_sub(1, std::memory_order_relaxed);
    (window_ ? g_window_bytes : g_buffer_bytes).fetch_sub(bytes_, std::memory_order_relaxed);
    buffer_.Reset();
    heap_.Reset();
}

bool enabled() {
    static const bool on = settings::get("G6SHIM_INPLACE").value_or("") == "1";
    return on;
}

void use_stretch_finder(FindStretch finder) { g_finder.store(finder, std::memory_order_release); }

std::shared_ptr<Region> place(ID3D12Device* device, std::uint64_t start, std::uint64_t bytes, bool window) {
    if (device == nullptr || bytes == 0) return nullptr;
    const std::uint64_t at = start & ~(kUnit - 1);
    const std::uint64_t end = (start + bytes + kUnit - 1) & ~(kUnit - 1);
    const std::uint64_t size = end - at;
    const std::uint64_t held = g_window_bytes.load(std::memory_order_relaxed) + g_buffer_bytes.load(std::memory_order_relaxed);
    if (held + size > most_bytes()) {
        g_over_budget.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (refused_before(at, size)) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const Kind kind = kind_of(at, size);
    std::uint64_t open_at = at;
    if (kind == Kind::kMapped) {
        const auto view = own_view(at, size);
        if (!view) {
            g_not_one_stretch.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        open_at = *view;
    } else if (kind != Kind::kPrivate) {
        g_not_one_kind.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    HRESULT result = S_OK;
    ComPtr<ID3D12Heap> heap = open_heap(device, open_at, size, &result);
    ComPtr<ID3D12Resource> buffer = heap ? place_buffer(device, heap.Get(), size, &result) : nullptr;
    if (!buffer) {
        if (kind == Kind::kMapped) ::UnmapViewOfFile(reinterpret_cast<void*>(open_at));
        remember_refused(at, size);
        if (worth_logging(g_refused))
            G6SHIM_WARN("d3d12_xs: title memory %llx+%llx not opened in place (0x%08lX); a copy instead",
                        static_cast<unsigned long long>(at), static_cast<unsigned long long>(size),
                        static_cast<unsigned long>(result));
        return nullptr;
    }
    host::name(buffer.Get(), window ? "title memory in place" : "title buffer in place");
    auto region = std::make_shared<Region>(std::move(heap), std::move(buffer), at, size,
                                           kind == Kind::kMapped ? Backing::kView : Backing::kPrivate, window);
    static std::atomic<std::uint64_t> said{0};
    if (worth_logging(said))
        G6SHIM_INFO("d3d12_xs: title %s %llx+%llx in place (%s memory)", window ? "memory" : "buffer",
                    static_cast<unsigned long long>(at), static_cast<unsigned long long>(size),
                    kind == Kind::kMapped ? "view" : "private");
    return region;
}

void title_memory_changed(std::uint64_t at, std::uint64_t bytes) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(g_changed_mutex);
    g_changed.emplace_back(at, at + bytes);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> take_changed() {
    std::lock_guard<std::mutex> lock(g_changed_mutex);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.swap(g_changed);
    return out;
}

Stats stats() {
    Stats out;
    out.windows = g_windows.load(std::memory_order_relaxed);
    out.window_bytes = g_window_bytes.load(std::memory_order_relaxed);
    out.buffers = g_buffers.load(std::memory_order_relaxed);
    out.buffer_bytes = g_buffer_bytes.load(std::memory_order_relaxed);
    out.not_one_kind = g_not_one_kind.load(std::memory_order_relaxed);
    out.not_one_stretch = g_not_one_stretch.load(std::memory_order_relaxed);
    out.refused = g_refused.load(std::memory_order_relaxed);
    out.over_budget = g_over_budget.load(std::memory_order_relaxed);
    out.let_go = g_let_go.load(std::memory_order_relaxed);
    return out;
}

void note_let_go(std::uint64_t count) { g_let_go.fetch_add(count, std::memory_order_relaxed); }

}  // namespace g6shim::d3d12_xs::inplace
