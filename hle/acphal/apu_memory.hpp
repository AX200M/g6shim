// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>

namespace g6shim::acphal {

inline constexpr std::uint64_t kApuAddressEnd = 0x1'0000'0000ull;
inline constexpr std::uint64_t kLowestApuAddress = 0x10000ull;
inline constexpr std::size_t kAllocationGranularity = 0x10000;

struct HeapSizes {
    std::uint32_t cached = 0;
    std::uint32_t uncached = 0;
};

inline bool is_power_of_two(std::uint64_t value) { return value && !(value & (value - 1)); }

inline void* reserve_below_apu_limit(std::size_t size, std::size_t alignment) {
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    static const auto virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2")));
    if (!virtual_alloc2) return nullptr;
    const std::size_t placement = alignment > kAllocationGranularity ? alignment : 0;
    const std::uint64_t lowest = placement ? (kLowestApuAddress + placement - 1) & ~std::uint64_t(placement - 1)
                                           : kLowestApuAddress;
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<PVOID>(lowest);
    requirements.HighestEndingAddress = reinterpret_cast<PVOID>(kApuAddressEnd - 1);
    requirements.Alignment = placement;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return virtual_alloc2(nullptr, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
}

class ApuMemory {
public:
    ApuMemory() = default;
    ApuMemory(const ApuMemory&) = delete;
    ApuMemory& operator=(const ApuMemory&) = delete;

    ~ApuMemory() {
        for (const auto& [base, size] : blocks_) VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
    }

    HRESULT create_heap(std::uint32_t cached, std::uint32_t uncached) {
        if (!cached && !uncached) return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(mutex_);
        heap_ = {cached, uncached};
        return S_OK;
    }

    HeapSizes heap() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_;
    }

    HRESULT allocate(std::uint32_t size, std::uint32_t alignment, void** va, std::uint32_t* apu) {
        if (va) *va = nullptr;
        if (apu) *apu = 0;
        if (!va || !size || (alignment && !is_power_of_two(alignment))) return E_INVALIDARG;
        void* block = reserve_below_apu_limit(size, alignment);
        if (!block) return E_OUTOFMEMORY;
        const auto base = reinterpret_cast<std::uintptr_t>(block);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            blocks_[base] = size;
        }
        *va = block;
        if (apu) *apu = static_cast<std::uint32_t>(base);
        return S_OK;
    }

    bool release(void* va) {
        const auto base = reinterpret_cast<std::uintptr_t>(va);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!blocks_.erase(base)) return false;
        }
        VirtualFree(va, 0, MEM_RELEASE);
        return true;
    }

    std::uint32_t apu_address(const void* va) const {
        const auto address = reinterpret_cast<std::uintptr_t>(va);
        std::lock_guard<std::mutex> lock(mutex_);
        return block_holding(address, 1) ? static_cast<std::uint32_t>(address) : 0;
    }

    void* virtual_address(std::uint32_t apu, std::uint32_t bytes) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return block_holding(apu, bytes ? bytes : 1) ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(apu))
                                                     : nullptr;
    }

    bool contains(const void* va, std::uint32_t bytes) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return block_holding(reinterpret_cast<std::uintptr_t>(va), bytes ? bytes : 1);
    }

    std::size_t block_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.size();
    }

private:
    bool block_holding(std::uintptr_t address, std::uint64_t bytes) const {
        auto it = blocks_.upper_bound(address);
        if (it == blocks_.begin()) return false;
        it = std::prev(it);
        return address - it->first + bytes <= it->second;
    }

    mutable std::mutex mutex_;
    std::map<std::uintptr_t, std::uint32_t> blocks_;
    HeapSizes heap_;
};

ApuMemory& apu_memory();

}  // namespace g6shim::acphal
