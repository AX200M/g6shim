// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "statistics.hpp"

#include <g6shim/log.hpp>

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace g6shim::xmem {

namespace {

std::atomic<std::size_t> g_gpu_optimal{kDefaultTitleMemory.gpu_optimal};
std::atomic<std::size_t> g_standard{kDefaultTitleMemory.standard};
std::atomic<std::size_t> g_transferred{0};

constexpr std::uint32_t kTitleToSystem = 0;
constexpr std::size_t kTransferGranularity = 2 * kMebibyte;

}  // namespace

TitleMemory title_memory() {
    return {g_gpu_optimal.load(std::memory_order_relaxed), g_standard.load(std::memory_order_relaxed)};
}

void set_title_memory(TitleMemory memory) {
    g_gpu_optimal.store(memory.gpu_optimal, std::memory_order_relaxed);
    g_standard.store(memory.standard, std::memory_order_relaxed);
}

int get_working_set_statistics(std::uint32_t, WorkingSet* out) {
    if (out == nullptr) return 0;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, counters.cb);

    const TitleMemory memory = title_memory();
    WorkingSet stats{};
    stats.title_limit = memory.gpu_optimal + memory.standard - g_transferred.load(std::memory_order_relaxed);
    stats.title_used = counters.PagefileUsage;
    stats.gpu_optimal_limit = memory.gpu_optimal;
    *out = stats;
    return 1;
}

int transfer_memory(std::uint32_t kind, std::size_t bytes) {
    G6SHIM_INFO("XMemTransferMemory kind=%u size=%zu", kind, bytes);
    if (bytes == 0) return 1;
    if (bytes > std::numeric_limits<std::size_t>::max() - (kTransferGranularity - 1)) return 0;
    const std::size_t rounded =
        (bytes + kTransferGranularity - 1) / kTransferGranularity * kTransferGranularity;
    if (kind != kTitleToSystem) return 0;
    const std::size_t standard = title_memory().standard;

    std::size_t current = g_transferred.load(std::memory_order_relaxed);
    for (;;) {
        if (current > standard || rounded > standard - current) return 0;
        if (g_transferred.compare_exchange_weak(current, current + rounded, std::memory_order_relaxed))
            return 1;
    }
}

}  // namespace g6shim::xmem

extern "C" BOOL WINAPI g6shim_XMemGetWorkingSetStatistics(DWORD which,
                                                          g6shim::xmem::WorkingSet* out) {
    return g6shim::xmem::get_working_set_statistics(which, out);
}

extern "C" BOOL WINAPI g6shim_XMemTransferMemory(DWORD kind, SIZE_T bytes) {
    return g6shim::xmem::transfer_memory(kind, bytes);
}
