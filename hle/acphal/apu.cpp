// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "apu_memory.hpp"

#include <g6shim/log.hpp>

#include <atomic>

namespace g6shim::acphal {

ApuMemory& apu_memory() {
    static ApuMemory memory;
    return memory;
}

}  // namespace g6shim::acphal

namespace {

using g6shim::acphal::apu_memory;

constexpr int kLoggedAllocations = 8;

void log_unknown_address(const char* function, const void* va) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < kLoggedAllocations)
        G6SHIM_WARN("%s(%p): not APU memory", function, va);
}

}  // namespace

extern "C" HRESULT WINAPI g6shim_ApuCreateHeap(UINT32 cached, UINT32 uncached) {
    const HRESULT hr = apu_memory().create_heap(cached, uncached);
    G6SHIM_INFO("ApuCreateHeap(cached 0x%X, uncached 0x%X) -> 0x%08lX", cached, uncached, hr);
    return hr;
}

extern "C" HRESULT WINAPI g6shim_ApuAlloc(void** va, UINT32* apu, UINT32 size, UINT32 alignment, UINT32 flags) {
    const HRESULT hr = apu_memory().allocate(size, alignment, va, apu);
    static std::atomic<int> logged{0};
    if (FAILED(hr) || logged.fetch_add(1, std::memory_order_relaxed) < kLoggedAllocations)
        G6SHIM_INFO("ApuAlloc(size 0x%X, alignment 0x%X, flags 0x%X) -> 0x%08lX, %p", size, alignment, flags, hr,
                    va ? *va : nullptr);
    return hr;
}

extern "C" void WINAPI g6shim_ApuFree(void* va) {
    if (va && !apu_memory().release(va)) log_unknown_address("ApuFree", va);
}

extern "C" UINT32 WINAPI g6shim_ApuMapVirtualAddress(const void* va) {
    const UINT32 apu = apu_memory().apu_address(va);
    if (!apu) log_unknown_address("ApuMapVirtualAddress", va);
    return apu;
}

extern "C" BOOLEAN WINAPI g6shim_ApuIsVirtualAddressValid(const void* va, UINT32 size) {
    return apu_memory().contains(va, size) ? TRUE : FALSE;
}
