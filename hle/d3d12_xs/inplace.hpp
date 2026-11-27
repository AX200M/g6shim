// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::inplace {

enum class Backing : std::uint8_t { kPrivate, kView };

class Region {
public:
    Region(Microsoft::WRL::ComPtr<ID3D12Heap> heap, Microsoft::WRL::ComPtr<ID3D12Resource> buffer,
           std::uint64_t base, std::uint64_t bytes, Backing backing, bool window);
    ~Region();
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    ID3D12Resource* buffer() const { return buffer_.Get(); }
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_ref() const { return buffer_; }
    std::uint64_t gpu(std::uint64_t address) const { return gpu_ + (address - base_); }
    std::uint64_t base() const { return base_; }
    std::uint64_t bytes() const { return bytes_; }
    Backing backing() const { return backing_; }

private:
    Microsoft::WRL::ComPtr<ID3D12Heap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::uint64_t base_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t gpu_ = 0;
    Backing backing_ = Backing::kPrivate;
    bool window_ = false;
};

using FindStretch = BOOL(WINAPI*)(std::uintptr_t address, std::size_t bytes, HANDLE* section, std::uint64_t* offset);

bool enabled();
void use_stretch_finder(FindStretch finder);
std::shared_ptr<Region> place(ID3D12Device* device, std::uint64_t start, std::uint64_t bytes, bool window);

void title_memory_changed(std::uint64_t at, std::uint64_t bytes);
std::vector<std::pair<std::uint64_t, std::uint64_t>> take_changed();

struct Stats {
    std::uint64_t windows = 0;
    std::uint64_t window_bytes = 0;
    std::uint64_t buffers = 0;
    std::uint64_t buffer_bytes = 0;
    std::uint64_t not_one_kind = 0;
    std::uint64_t not_one_stretch = 0;
    std::uint64_t refused = 0;
    std::uint64_t over_budget = 0;
    std::uint64_t let_go = 0;
};

Stats stats();
void note_let_go(std::uint64_t count);

}  // namespace g6shim::d3d12_xs::inplace
