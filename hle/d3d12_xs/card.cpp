// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "card.hpp"

#include "g_card_piece_dxil.h"
#include "host.hpp"
#include "removal.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <utility>

namespace g6shim::d3d12_xs::card {

namespace {

constexpr std::uint64_t kGiB = 1ull << 30;
constexpr std::uint64_t kTwelveGiB = 12ull * kGiB;
constexpr std::uint64_t kTwentyGiB = 20ull * kGiB;
constexpr std::uint32_t kModeStore = 0;
constexpr std::uint32_t kModeCompare = 1;
constexpr std::uint32_t kRootSource = 0;
constexpr std::uint32_t kRootHashes = 1;
constexpr std::uint32_t kRootFlags = 2;
constexpr std::uint32_t kRootSpan = 3;
constexpr std::uint32_t kSpanDwords = 4;

struct Span {
    std::uint32_t first = 0;
    std::uint32_t bytes = 0;
    std::uint32_t mode = 0;
    std::uint32_t piece_bytes = static_cast<std::uint32_t>(kPieceBytes);
};

static_assert(sizeof(Span) == kSpanDwords * sizeof(std::uint32_t));

struct HashPipeline {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    bool failed = false;
};

struct Recorder {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    std::uint64_t value = 0;
    bool open = false;

    ~Recorder() {
        if (event != nullptr) ::CloseHandle(event);
    }

    ID3D12GraphicsCommandList* begin(ID3D12Device* device) {
        if (device == nullptr) return nullptr;
        if (!allocator) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                return nullptr;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&list))))
                return nullptr;
            list->Close();
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return nullptr;
            event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event == nullptr) return nullptr;
        }
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return nullptr;
        open = true;
        return list.Get();
    }

    ID3D12CommandList* end() {
        if (!open) return nullptr;
        if (FAILED(list->Close())) {
            open = false;
            return nullptr;
        }
        open = false;
        return list.Get();
    }

    void submit(ID3D12CommandQueue* queue, ID3D12CommandList* commands) {
        if (queue == nullptr || commands == nullptr || !fence || event == nullptr) return;
        queue->ExecuteCommandLists(1, &commands);
        ++value;
        if (FAILED(queue->Signal(fence.Get(), value))) return;
        if (FAILED(fence->SetEventOnCompletion(value, event))) return;
        ::WaitForSingleObject(event, INFINITE);
    }
};

std::mutex g_mutex;
std::mutex g_recorder_mutex;
std::condition_variable g_recorder_ready;
bool g_recorder_busy = false;
std::vector<std::weak_ptr<Side>> g_live;
std::atomic<std::uint64_t> g_reserved{0};
Recorder g_recorder;
HashPipeline g_hash;

bool ensure_pipeline(ID3D12Device* device) {
    if (g_hash.pipeline) return true;
    if (g_hash.failed || device == nullptr) return false;
    D3D12_ROOT_PARAMETER parameters[4]{};
    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[kRootSpan].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kRootSpan].Constants.ShaderRegister = 0;
    parameters[kRootSpan].Constants.Num32BitValues = kSpanDwords;
    parameters[kRootSpan].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(::D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_hash.signature)))) {
        g_hash.failed = true;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = g_hash.signature.Get();
    pipeline_desc.CS.pShaderBytecode = g_card_piece_dxil;
    pipeline_desc.CS.BytecodeLength = sizeof(g_card_piece_dxil);
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&g_hash.pipeline)))) {
        g_hash.failed = true;
        return false;
    }
    return true;
}

bool reserve(std::uint64_t bytes, std::uint64_t maximum) {
    std::uint64_t current = g_reserved.load(std::memory_order_relaxed);
    while (bytes <= maximum && current <= maximum - bytes) {
        if (g_reserved.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel)) return true;
    }
    return false;
}

void release_reserved(std::uint64_t bytes) {
    if (bytes == 0) return;
    g_reserved.fetch_sub(bytes, std::memory_order_acq_rel);
}

std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> local_memory(ID3D12Device* device) {
    if (device == nullptr) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(&adapter))) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(adapter.As(&adapter3))) return std::nullopt;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return std::nullopt;
    return info;
}

Limits limits_from(std::optional<std::uint64_t> budget) {
    Limits out;
    if (!budget || *budget <= kTwelveGiB) return out;
    if (*budget < kTwentyGiB) {
        out.minimum = 4ull * kMiB;
        out.maximum = 2048ull * kMiB;
        return out;
    }
    const std::uint64_t quarter = (*budget / 4 / kMiB) * kMiB;
    out.minimum = kMiB;
    out.maximum = std::min(quarter, kAddressableMax);
    if (out.maximum < out.minimum) out.maximum = out.minimum;
    return out;
}

Microsoft::WRL::ComPtr<ID3D12Resource> committed(ID3D12Device* device, D3D12_HEAP_TYPE heap, std::uint64_t bytes,
                                                 D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(bytes, 1);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    if (resource == nullptr || before == after) return;
    const D3D12_RESOURCE_BARRIER desc = host::transition(resource, before, after);
    list->ResourceBarrier(1, &desc);
}

std::vector<std::uint32_t> pieces_covering(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
                                           std::uint64_t bytes) {
    std::vector<std::uint32_t> pieces;
    for (const auto& [from, to] : ranges) {
        if (from >= to || from >= bytes) continue;
        const std::uint64_t end = std::min(to, bytes);
        const std::uint32_t first = static_cast<std::uint32_t>(from / kPieceBytes);
        const std::uint32_t last = static_cast<std::uint32_t>((end - 1) / kPieceBytes);
        for (std::uint32_t piece = first; piece <= last; ++piece) pieces.push_back(piece);
    }
    std::sort(pieces.begin(), pieces.end());
    pieces.erase(std::unique(pieces.begin(), pieces.end()), pieces.end());
    return pieces;
}

void coalesce(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t offset,
              std::uint64_t length) {
    if (length == 0) return;
    const std::uint64_t end = offset + length;
    for (auto& range : ranges) {
        if (range.second < offset || range.first > end) continue;
        range.first = std::min(range.first, offset);
        range.second = std::max(range.second, end);
        return;
    }
    ranges.emplace_back(offset, end);
}

}  // namespace

bool enabled() {
    const std::optional<std::string> value = settings::get("G6SHIM_CARD_BUFFERS");
    if (!value || value->empty()) return true;
    return value->front() != '0';
}

Limits limits(ID3D12Device* device) {
    static const Limits cached = limits_from([&]() -> std::optional<std::uint64_t> {
        const auto info = local_memory(device);
        if (!info) return std::nullopt;
        return info->Budget;
    }());
    return cached;
}

void log_budget(ID3D12Device* device) {
    const auto info = local_memory(device);
    if (!info) {
        G6SHIM_INFO("d3d12_xs: local video memory budget unavailable");
        return;
    }
    G6SHIM_INFO("d3d12_xs: local video memory budget %llu MiB, in use %llu MiB",
                static_cast<unsigned long long>(info->Budget / kMiB),
                static_cast<unsigned long long>(info->CurrentUsage / kMiB));
}

std::uint64_t minimum_bytes(ID3D12Device* device) { return limits(device).minimum; }

std::uint32_t Side::pieces() const {
    if (rounded_ == 0) return 0;
    return static_cast<std::uint32_t>(rounded_ / kPieceBytes);
}

std::uint64_t Side::piece_bytes(std::uint32_t piece) const {
    const std::uint64_t start = static_cast<std::uint64_t>(piece) * kPieceBytes;
    if (start >= bytes_) return 0;
    return std::min(kPieceBytes, bytes_ - start);
}

bool Side::allocate(ID3D12Device* device, std::uint64_t bytes, std::uint64_t rounded) {
    bytes_ = bytes;
    rounded_ = rounded;
    buffer_ = committed(device, D3D12_HEAP_TYPE_DEFAULT, rounded, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COMMON);
    staging_ = committed(device, D3D12_HEAP_TYPE_UPLOAD, rounded, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    const std::uint64_t hash_bytes = (rounded / kPieceBytes) * 16;
    const std::uint64_t flag_bytes = (rounded / kPieceBytes) * 4;
    hashes_ = committed(device, D3D12_HEAP_TYPE_DEFAULT, hash_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flags_ = committed(device, D3D12_HEAP_TYPE_DEFAULT, flag_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flag_readback_ = committed(device, D3D12_HEAP_TYPE_READBACK, flag_bytes, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
    if (!buffer_ || !staging_ || !hashes_ || !flags_ || !flag_readback_) return false;
    void* staging_map = nullptr;
    void* flag_map = nullptr;
    if (FAILED(staging_->Map(0, nullptr, &staging_map)) || FAILED(flag_readback_->Map(0, nullptr, &flag_map)))
        return false;
    staging_map_ = static_cast<std::uint8_t*>(staging_map);
    flag_map_ = static_cast<std::uint8_t*>(flag_map);
    std::memset(staging_map_, 0, static_cast<std::size_t>(rounded));
    gpu_ = buffer_->GetGPUVirtualAddress();
    host::name(buffer_.Get(), "card buffer");
    host::name(staging_.Get(), "card staging");
    return ensure_pipeline(device);
}

std::shared_ptr<Side> Side::try_create(ID3D12Device* device, std::uint64_t bytes) {
    if (!enabled() || device == nullptr || bytes == 0) return nullptr;
    const Limits cap = limits(device);
    if (bytes < cap.minimum) return nullptr;
    const std::uint64_t rounded = (bytes + kPieceBytes - 1) / kPieceBytes * kPieceBytes;
    if (rounded > cap.maximum || rounded > kAddressableMax) return nullptr;
    if (!reserve(rounded, cap.maximum)) return nullptr;
    auto side = std::shared_ptr<Side>(new Side());
    side->reserved_ = rounded;
    if (!side->allocate(device, bytes, rounded)) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_live.push_back(side);
    }
    log_budget(device);
    G6SHIM_INFO("d3d12_xs: title buffer %llu MiB kept in video memory (%llu MiB of %llu MiB card copies)",
                static_cast<unsigned long long>(bytes / kMiB),
                static_cast<unsigned long long>(g_reserved.load(std::memory_order_relaxed) / kMiB),
                static_cast<unsigned long long>(cap.maximum / kMiB));
    return side;
}

Side::~Side() { release_reserved(reserved_); }

const std::uint8_t* Side::fetched(std::uint32_t piece) const {
    if (piece_map_ == nullptr) return nullptr;
    for (std::size_t i = 0; i < fetched_.size(); ++i)
        if (fetched_[i] == piece) return piece_map_ + i * kPieceBytes;
    return nullptr;
}

void Side::note_upload(std::uint64_t offset, std::uint64_t length) { coalesce(uploads_, offset, length); }

void Side::note_writes(std::uint64_t offset, std::uint64_t length) {
    if (length == 0 || offset >= bytes_) return;
    const std::uint64_t end = std::min(offset + length, bytes_);
    const std::uint32_t first = static_cast<std::uint32_t>(offset / kPieceBytes);
    const std::uint32_t last = static_cast<std::uint32_t>((end - 1) / kPieceBytes);
    for (std::uint32_t piece = first; piece <= last; ++piece) watched_.push_back(piece);
}

void Side::note_kept(std::uint32_t piece, std::uint64_t offset, std::uint64_t length) {
    coalesce(uploads_, offset, length);
    rehash_.push_back(piece);
}

void Side::transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES after) {
    barrier(list, buffer_.Get(), buffer_state_, after);
    buffer_state_ = after;
}

void Side::bind(ID3D12GraphicsCommandList* list) {
    list->SetComputeRootSignature(g_hash.signature.Get());
    list->SetPipelineState(g_hash.pipeline.Get());
    list->SetComputeRootUnorderedAccessView(kRootSource, buffer_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(kRootHashes, hashes_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(kRootFlags, flags_->GetGPUVirtualAddress());
}

void Side::hash_runs(ID3D12GraphicsCommandList* list, const std::vector<std::uint32_t>& pieces, std::uint32_t mode) {
    if (pieces.empty() || !g_hash.pipeline) return;
    bind(list);
    std::size_t run = 0;
    for (std::size_t i = 0; i <= pieces.size(); ++i) {
        const bool split = i == pieces.size() || (i > run && pieces[i] != pieces[i - 1] + 1);
        if (i == run || !split) continue;
        Span span;
        span.first = pieces[run];
        span.bytes = static_cast<std::uint32_t>(std::min(bytes_, kAddressableMax));
        span.mode = mode;
        span.piece_bytes = static_cast<std::uint32_t>(kPieceBytes);
        list->SetComputeRoot32BitConstants(kRootSpan, kSpanDwords, &span, 0);
        removal::mark(list, "card copy");
        list->Dispatch(pieces[i - 1] - pieces[run] + 1, 1, 1);
        run = i;
    }
}

bool Side::prepare(ID3D12GraphicsCommandList* list) {
    if (!baseline_) coalesce(uploads_, 0, rounded_);
    if (uploads_.empty()) return false;
    const auto pieces = pieces_covering(uploads_, bytes_);
    transition(list, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& [from, to] : uploads_) {
        if (from >= to || from >= rounded_) continue;
        const std::uint64_t length = std::min(to, rounded_) - from;
        list->CopyBufferRegion(buffer_.Get(), from, staging_.Get(), from, length);
    }
    uploads_.clear();
    transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!pieces.empty()) hash_runs(list, pieces, kModeStore);
    else if (!baseline_) {
        std::vector<std::uint32_t> all(this->pieces());
        for (std::uint32_t piece = 0; piece < all.size(); ++piece) all[piece] = piece;
        hash_runs(list, all, kModeStore);
    }
    transition(list, D3D12_RESOURCE_STATE_COMMON);
    baseline_ = true;
    return true;
}

bool Side::compare(ID3D12GraphicsCommandList* list) {
    if (!baseline_ || watched_.empty() || !g_hash.pipeline) return false;
    std::sort(watched_.begin(), watched_.end());
    watched_.erase(std::unique(watched_.begin(), watched_.end()), watched_.end());
    transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hash_runs(list, watched_, kModeCompare);
    transition(list, D3D12_RESOURCE_STATE_COMMON);
    barrier(list, flags_.Get(), flag_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    flag_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->CopyBufferRegion(flag_readback_.Get(), 0, flags_.Get(), 0, static_cast<std::uint64_t>(pieces()) * 4);
    barrier(list, flags_.Get(), flag_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flag_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

void Side::read_flags() {
    flagged_.clear();
    fetched_.clear();
    if (flag_map_ == nullptr) return;
    for (const std::uint32_t piece : watched_) {
        std::uint32_t flag = 0;
        std::memcpy(&flag, flag_map_ + static_cast<std::size_t>(piece) * 4, sizeof flag);
        if ((flag & 1u) != 0) flagged_.push_back(piece);
    }
    watched_.clear();
}


bool Side::sync(ID3D12GraphicsCommandList* list) {
    std::vector<std::uint32_t> pieces = pieces_covering(uploads_, bytes_);
    for (const std::uint32_t piece : flagged_) pieces.push_back(piece);
    pieces.insert(pieces.end(), rehash_.begin(), rehash_.end());
    std::sort(pieces.begin(), pieces.end());
    pieces.erase(std::unique(pieces.begin(), pieces.end()), pieces.end());
    const bool copying = !uploads_.empty();
    if (!copying && pieces.empty()) return false;
    if (copying) {
        transition(list, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const auto& [from, to] : uploads_) {
            if (from >= to || from >= rounded_) continue;
            list->CopyBufferRegion(buffer_.Get(), from, staging_.Get(), from, std::min(to, rounded_) - from);
        }
        uploads_.clear();
        transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
        transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    hash_runs(list, pieces, kModeStore);
    transition(list, D3D12_RESOURCE_STATE_COMMON);
    rehash_.clear();
    flagged_.clear();
    return true;
}

Batch take_batch() {
    Batch batch;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_live.erase(std::remove_if(g_live.begin(), g_live.end(),
                                [](const std::weak_ptr<Side>& side) { return side.expired(); }),
                 g_live.end());
    batch.sides_.reserve(g_live.size());
    for (const auto& side : g_live)
        if (auto live = side.lock()) batch.sides_.push_back(std::move(live));
    return batch;
}

bool Batch::needs_prepare() const {
    for (const auto& side : sides_)
        if (side && (!side->baseline_ || !side->uploads_.empty())) return true;
    return false;
}

bool Batch::needs_readback() const {
    for (const auto& side : sides_)
        if (side && side->baseline_ && !side->watched_.empty()) return true;
    return false;
}

ID3D12CommandList* Batch::prepare(ID3D12Device* device) {
    if (!needs_prepare()) return nullptr;
    ID3D12GraphicsCommandList* list = g_recorder.begin(device);
    if (list == nullptr) return nullptr;
    bool any = false;
    for (const auto& side : sides_) any = side->prepare(list) || any;
    if (!any) {
        g_recorder.end();
        return nullptr;
    }
    return g_recorder.end();
}

ID3D12CommandList* Batch::compare(ID3D12Device* device) {
    ID3D12GraphicsCommandList* list = g_recorder.begin(device);
    if (list == nullptr) return nullptr;
    bool any = false;
    for (const auto& side : sides_) any = side->compare(list) || any;
    if (!any) {
        g_recorder.end();
        return nullptr;
    }
    return g_recorder.end();
}

void Batch::fetch(ID3D12Device* device, ID3D12CommandQueue* queue) {
    for (const auto& side : sides_) side->read_flags();
    ID3D12GraphicsCommandList* list = g_recorder.begin(device);
    if (list == nullptr) return;
    bool any = false;
    for (const auto& side : sides_) {
        if (side->flagged_.empty()) continue;
        const std::uint64_t need = static_cast<std::uint64_t>(side->flagged_.size()) * kPieceBytes;
        if (!side->piece_readback_ || side->piece_readback_bytes_ < need) {
            side->piece_readback_ = committed(device, D3D12_HEAP_TYPE_READBACK, need, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
            side->piece_readback_bytes_ = side->piece_readback_ ? need : 0;
            side->piece_map_ = nullptr;
            if (side->piece_readback_) {
                void* mapped = nullptr;
                if (SUCCEEDED(side->piece_readback_->Map(0, nullptr, &mapped)))
                    side->piece_map_ = static_cast<std::uint8_t*>(mapped);
            }
        }
        if (!side->piece_readback_) continue;
        side->transition(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (std::size_t i = 0; i < side->flagged_.size(); ++i) {
            const std::uint32_t piece = side->flagged_[i];
            const std::uint64_t length = side->piece_bytes(piece);
            if (length == 0) continue;
            list->CopyBufferRegion(side->piece_readback_.Get(), i * kPieceBytes, side->buffer_.Get(),
                                   static_cast<std::uint64_t>(piece) * kPieceBytes, length);
        }
        side->transition(list, D3D12_RESOURCE_STATE_COMMON);
        side->fetched_ = side->flagged_;
        any = true;
    }
    ID3D12CommandList* closed = g_recorder.end();
    if (any && closed != nullptr) g_recorder.submit(queue, closed);
}

void Batch::sync_kept(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ID3D12GraphicsCommandList* list = g_recorder.begin(device);
    if (list == nullptr) return;
    bool any = false;
    for (const auto& side : sides_) any = side->sync(list) || any;
    ID3D12CommandList* closed = g_recorder.end();
    if (any && closed != nullptr) g_recorder.submit(queue, closed);
}

void Batch::read_changed(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (ID3D12CommandList* compared = compare(device)) g_recorder.submit(queue, compared);
    fetch(device, queue);
}

void wait_recorder() {
    std::unique_lock<std::mutex> lock(g_recorder_mutex);
    g_recorder_ready.wait(lock, [] { return !g_recorder_busy; });
}

void mark_recorder_busy() {
    std::lock_guard<std::mutex> lock(g_recorder_mutex);
    g_recorder_busy = true;
}

void release_recorder() {
    {
        std::lock_guard<std::mutex> lock(g_recorder_mutex);
        g_recorder_busy = false;
    }
    g_recorder_ready.notify_all();
}
}  // namespace g6shim::d3d12_xs::card
