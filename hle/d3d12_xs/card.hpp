// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace g6shim::d3d12_xs::card {

inline constexpr std::uint64_t kPieceBytes = 64ull << 10;
inline constexpr std::uint64_t kMiB = 1ull << 20;
inline constexpr std::uint64_t kAddressableMax = 2048ull * kMiB;
inline constexpr std::uint32_t kHashLanes = 64;

struct Limits {
    std::uint64_t minimum = 16ull * kMiB;
    std::uint64_t maximum = 512ull * kMiB;
};

bool enabled();
Limits limits(ID3D12Device* device);
void log_budget(ID3D12Device* device);
std::uint64_t minimum_bytes(ID3D12Device* device);

class Side;

class Batch {
public:
    bool needs_prepare() const;
    bool needs_readback() const;
    ID3D12CommandList* prepare(ID3D12Device* device);
    ID3D12CommandList* compare(ID3D12Device* device);
    void read_changed(ID3D12Device* device, ID3D12CommandQueue* queue);
    void fetch(ID3D12Device* device, ID3D12CommandQueue* queue);
    void sync_kept(ID3D12Device* device, ID3D12CommandQueue* queue);

private:
    std::vector<std::shared_ptr<Side>> sides_;
    friend Batch take_batch();
};

Batch take_batch();
void wait_recorder();
void mark_recorder_busy();
void release_recorder();


class Side {
public:
    static std::shared_ptr<Side> try_create(ID3D12Device* device, std::uint64_t bytes);
    ~Side();

    Side(const Side&) = delete;
    Side& operator=(const Side&) = delete;

    ID3D12Resource* buffer() const { return buffer_.Get(); }
    std::uint8_t* staging() const { return staging_map_; }
    std::uint64_t gpu() const { return gpu_; }
    std::uint64_t bytes() const { return bytes_; }
    std::size_t fetched_count() const { return fetched_.size(); }
    const std::uint8_t* fetched(std::uint32_t piece) const;

    void note_upload(std::uint64_t offset, std::uint64_t length);
    void note_writes(std::uint64_t offset, std::uint64_t length);
    void note_kept(std::uint32_t piece, std::uint64_t offset, std::uint64_t length);

private:
    friend class Batch;
    Side() = default;

    bool allocate(ID3D12Device* device, std::uint64_t bytes, std::uint64_t rounded);
    bool prepare(ID3D12GraphicsCommandList* list);
    bool compare(ID3D12GraphicsCommandList* list);
    void read_flags();
    bool sync(ID3D12GraphicsCommandList* list);
    void bind(ID3D12GraphicsCommandList* list);
    void hash_runs(ID3D12GraphicsCommandList* list, const std::vector<std::uint32_t>& pieces, std::uint32_t mode);
    void transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES after);
    std::uint32_t pieces() const;
    std::uint64_t piece_bytes(std::uint32_t piece) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> staging_;
    Microsoft::WRL::ComPtr<ID3D12Resource> hashes_;
    Microsoft::WRL::ComPtr<ID3D12Resource> flags_;
    Microsoft::WRL::ComPtr<ID3D12Resource> flag_readback_;
    Microsoft::WRL::ComPtr<ID3D12Resource> piece_readback_;
    std::uint8_t* staging_map_ = nullptr;
    std::uint8_t* flag_map_ = nullptr;
    std::uint8_t* piece_map_ = nullptr;
    std::uint64_t piece_readback_bytes_ = 0;
    std::uint64_t gpu_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t rounded_ = 0;
    std::uint64_t reserved_ = 0;
    D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES flag_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool baseline_ = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> uploads_;
    std::vector<std::uint32_t> watched_;
    std::vector<std::uint32_t> flagged_;
    std::vector<std::uint32_t> fetched_;
    std::vector<std::uint32_t> rehash_;
};

}  // namespace g6shim::d3d12_xs::card
