// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "host.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::shadow {

inline constexpr std::uint64_t kIdleSubmissions = 512;

struct LooseStats {
    std::size_t windows = 0;
    std::uint64_t bytes = 0;
    std::uint64_t evicted = 0;
};

class Shadow : public std::enable_shared_from_this<Shadow> {
public:
    Shadow(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::uint8_t* data, std::uint64_t gpu,
           std::uint64_t start, std::uint64_t bytes);

    std::uint64_t gpu(std::uint64_t address) const { return gpu_ + (address - start_); }
    std::uint64_t left(std::uint64_t address) const;
    std::uint64_t offset(std::uint64_t address) const { return address - start_; }
    std::uint64_t start() const { return start_; }
    std::uint64_t bytes() const { return bytes_; }
    std::uint8_t* data() const { return data_; }
    ID3D12Resource* buffer() const { return buffer_.Get(); }
    void used(std::uint64_t submission) { last_used_.store(submission, std::memory_order_relaxed); }
    std::uint64_t last_used() const { return last_used_.load(std::memory_order_relaxed); }

    void refresh(host::HostList& list, std::uint64_t address, std::uint64_t bytes);
    void gpu_writes(host::HostList& list, std::uint64_t address, std::uint64_t bytes);
    void writing(std::size_t address, std::size_t bytes);
    std::size_t copy_back(std::size_t address, std::size_t bytes);
    void discard_write(std::size_t address, std::size_t bytes);
    std::size_t update(std::size_t address, std::size_t bytes);

private:
    std::pair<std::size_t, std::size_t> inside(std::size_t address, std::size_t bytes) const;
    std::size_t update_free(std::size_t from, std::size_t to);
    std::size_t copy_stale(std::size_t from, std::size_t to, std::uint64_t now,
                           const std::vector<std::pair<std::size_t, std::uint64_t>>& written);
    std::size_t copy_range(std::size_t from, std::size_t to);

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::uint8_t* data_ = nullptr;
    std::uint64_t gpu_ = 0;
    std::uint64_t start_ = 0;
    std::uint64_t bytes_ = 0;
    std::mutex copied_mutex_;
    std::vector<std::uint64_t> copied_;
    std::atomic<std::uint8_t> watched_{0};
    std::mutex pending_mutex_;
    std::vector<std::pair<std::size_t, std::size_t>> pending_;
    std::mutex before_mutex_;
    std::optional<std::pair<std::size_t, std::vector<std::uint8_t>>> before_;
    std::atomic<std::uint64_t> last_used_{0};
};

void register_buffer(std::uintptr_t resource, std::uint64_t start, std::uint64_t bytes);
void unregister_buffer(std::uintptr_t resource, std::uint64_t start);
std::optional<std::shared_ptr<Shadow>> find(std::uint64_t address, std::uint64_t bytes);
std::string nearest(std::uint64_t va);
void submission_completed();
std::uint64_t eviction_generation();
bool live_loose(ID3D12Resource* buffer);
LooseStats loose_stats();

}  // namespace g6shim::d3d12_xs::shadow
