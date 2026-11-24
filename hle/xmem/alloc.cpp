// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "alloc.hpp"
#include "virtual_memory.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <malloc.h>

namespace g6shim::xmem {

namespace {

constexpr char kHooksExport[] = "__XMemAllocationHooks__";

struct AllocationHooks {
    void* (WINAPI* allocate)(std::size_t bytes, std::uint64_t attributes);
    void (WINAPI* free)(void* block, std::uint64_t attributes);
};

AllocationHooks read_allocation_hooks() {
    AllocationHooks hooks{};
    const HMODULE image = GetModuleHandleW(nullptr);
    const void* const* exported =
        image ? reinterpret_cast<const void* const*>(GetProcAddress(image, kHooksExport)) : nullptr;
    G6SHIM_INFO("xmem allocation hooks: %s", exported ? "found" : "not found");
    if (!exported) return hooks;
    hooks.allocate = reinterpret_cast<void* (WINAPI*)(std::size_t, std::uint64_t)>(const_cast<void*>(exported[0]));
    hooks.free = reinterpret_cast<void (WINAPI*)(void*, std::uint64_t)>(const_cast<void*>(exported[1]));
    return hooks;
}

const AllocationHooks& allocation_hooks() {
    static const AllocationHooks hooks = read_allocation_hooks();
    return hooks;
}

}  // namespace

std::size_t allocation_alignment(std::uint64_t attributes) {
    const std::uint64_t field = (attributes >> kAllocationFieldShift) & kAllocationFieldMask;
    return std::size_t{1} << (field < kMinimumAllocationField ? kMinimumAllocationField : field);
}

}  // namespace g6shim::xmem

extern "C" void* WINAPI g6shim_XMemAllocDefault(SIZE_T bytes, ULONGLONG attributes);
extern "C" void WINAPI g6shim_XMemFreeDefault(void* block, ULONGLONG attributes);

extern "C" void* WINAPI g6shim_XMemAlloc(SIZE_T bytes, ULONGLONG attributes) {
    if (const auto hook = g6shim::xmem::allocation_hooks().allocate) return hook(bytes, attributes);
    return g6shim_XMemAllocDefault(bytes, attributes);
}

extern "C" void WINAPI g6shim_XMemFree(void* block, ULONGLONG attributes) {
    if (const auto hook = g6shim::xmem::allocation_hooks().free) {
        hook(block, attributes);
        return;
    }
    g6shim_XMemFreeDefault(block, attributes);
}

extern "C" void* WINAPI g6shim_XMemAllocDefault(SIZE_T bytes, ULONGLONG attributes) {
    if (attributes & g6shim::xmem::k32BitAllocation)
        return g6shim::xmem::allocate_virtual(nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                                              g6shim::xmem::kGraphicsFlags | g6shim::xmem::kAddress32BitFlags,
                                              PAGE_READWRITE);
    return _aligned_malloc(bytes ? bytes : 1, g6shim::xmem::allocation_alignment(attributes));
}

extern "C" void WINAPI g6shim_XMemFreeDefault(void* block, ULONGLONG attributes) {
    if (!block) return;
    if (attributes & g6shim::xmem::k32BitAllocation) {
        VirtualFree(block, 0, MEM_RELEASE);
        return;
    }
    _aligned_free(block);
}

extern "C" BOOL WINAPI g6shim_XMemIsToolsMemAvailable() { return 0; }
