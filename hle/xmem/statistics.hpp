// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "virtual_memory.hpp"

#include <cstddef>
#include <cstdint>

namespace g6shim::xmem {

inline constexpr std::size_t kMebibyte = std::size_t{1} << 20;
inline constexpr std::size_t kGibibyte = std::size_t{1} << 30;

struct TitleMemory {
    std::size_t gpu_optimal;
    std::size_t standard;
};

inline constexpr TitleMemory kDefaultTitleMemory{10 * kGibibyte, 7 * kGibibyte / 2};
inline constexpr TitleMemory kSmallTitleMemory{0, 7776 * kMebibyte};

TitleMemory title_memory();
void set_title_memory(TitleMemory memory);

struct WorkingSet {
    std::size_t title_limit;
    std::size_t title_used;
    std::size_t tools_limit;
    std::size_t tools_used;
    std::uint32_t stack_used;
    std::uint32_t physical_pages;
    std::size_t gpu_optimal_limit;
    std::size_t gpu_optimal_used;
    std::size_t tools_gpu_optimal_limit;
    std::size_t tools_gpu_optimal_used;
    std::uint32_t kernel_pool_used;
    std::uint32_t kernel_pool_limit;
};
static_assert(sizeof(WorkingSet) == 0x50);
static_assert(offsetof(WorkingSet, gpu_optimal_limit) == 0x28);
static_assert(offsetof(WorkingSet, kernel_pool_used) == 0x48);

int get_working_set_statistics(std::uint32_t which, WorkingSet* out);
int transfer_memory(std::uint32_t kind, std::size_t bytes);

}  // namespace g6shim::xmem
