// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "alloc.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

std::atomic<int> g_hook_allocs{0};
std::atomic<int> g_hook_frees{0};

}  // namespace

extern "C" void* WINAPI g6shim_XMemAllocDefault(SIZE_T bytes, ULONGLONG attributes);
extern "C" void WINAPI g6shim_XMemFreeDefault(void* block, ULONGLONG attributes);

extern "C" void* WINAPI forwarding_allocate(std::size_t bytes, std::uint64_t attributes) {
    ++g_hook_allocs;
    return g6shim_XMemAllocDefault(bytes, attributes);
}

extern "C" void WINAPI forwarding_free(void* block, std::uint64_t attributes) {
    ++g_hook_frees;
    g6shim_XMemFreeDefault(block, attributes);
}

extern "C" std::uintptr_t __XMemAllocationHooks__[] = {
    reinterpret_cast<std::uintptr_t>(&forwarding_allocate),
    reinterpret_cast<std::uintptr_t>(&forwarding_free)};

extern "C" void* WINAPI g6shim_XMemAlloc(SIZE_T bytes, ULONGLONG attributes);
extern "C" void WINAPI g6shim_XMemFree(void* block, ULONGLONG attributes);
extern "C" BOOL WINAPI g6shim_XMemIsToolsMemAvailable();

TEST(xmem_alignment_field_decodes) {
    using namespace g6shim::xmem;
    CHECK(allocation_alignment(0) == 16);
    CHECK(allocation_alignment(std::uint64_t{4} << kAllocationFieldShift) == 16);
    CHECK(allocation_alignment(std::uint64_t{0xc} << kAllocationFieldShift) == 4096);
    CHECK(allocation_alignment(std::uint64_t{0x1f} << kAllocationFieldShift) == (std::size_t{1} << 31));
    CHECK(allocation_alignment(k32BitAllocation) == 16);
}

TEST(xmem_allocations_honour_the_alignment_field) {
    using namespace g6shim::xmem;
    const std::uint64_t graphics_type = std::uint64_t{1} << 29;
    const std::uint64_t fields[] = {0, 4, 0xc, 0x10};
    const std::size_t alignments[] = {16, 16, 4096, 64 * 1024};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint64_t attributes = fields[i] << kAllocationFieldShift | graphics_type;
        void* block = g6shim_XMemAlloc(100, attributes);
        CHECK(block != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(block) % alignments[i] == 0);
        if (block) std::memset(block, 0xab, 100);
        g6shim_XMemFree(block, attributes);
    }
    void* low = g6shim_XMemAlloc(4096, k32BitAllocation);
    CHECK(low != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(low) < (std::uintptr_t{1} << 32));
    g6shim_XMemFree(low, k32BitAllocation);
    CHECK(g6shim_XMemIsToolsMemAvailable() == 0);
}

TEST(xmem_allocation_hooks_receive_the_calls) {
    const int allocs = g_hook_allocs.load();
    const int frees = g_hook_frees.load();
    void* block = g6shim_XMemAlloc(64, 0);
    g6shim_XMemFree(block, 0);
    CHECK(g_hook_allocs.load() == allocs + 1);
    CHECK(g_hook_frees.load() == frees + 1);
}

TEST(xmem_default_allocations_free_with_matching_attributes) {
    void* block = g6shim_XMemAllocDefault(4096, 0);
    CHECK(block != nullptr);
    std::memset(block, 0x5a, 4096);
    g6shim_XMemFreeDefault(block, 0);
    g6shim_XMemFreeDefault(nullptr, 0);
}
