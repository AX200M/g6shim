// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace g6shim::d3d12 {

struct GraphicsHead {
    const void* const* table;
    std::uint32_t device_bits;
    std::atomic<std::uint32_t> refs;
};
static_assert(sizeof(GraphicsHead) == 0x10);
static_assert(offsetof(GraphicsHead, refs) == 0xC);

inline constexpr std::size_t kQueryInterfaceSlot = 0;
inline constexpr std::size_t kAddRefSlot = 1;
inline constexpr std::size_t kReleaseSlot = 2;
inline constexpr std::size_t kMaxTableSlots = 256;
inline constexpr std::size_t kMaxPcObjectOffset = 0x1000;

using SlotOverride = std::pair<std::size_t, const void*>;

struct TableSpec {
    const char* kind;
    std::size_t pc_object_offset;
    std::size_t pc_slots;
    std::size_t slots;
};

const void* const* build_table(const TableSpec& spec, std::initializer_list<SlotOverride> overrides);
const void* const* build_table(const TableSpec& spec, const std::vector<SlotOverride>& overrides);

const void* forward_thunk(std::size_t pc_object_offset, std::size_t slot);

const char* table_kind(const void* object);

inline constexpr std::size_t kRecordedCalls = 32;

void record_call(const void* object, std::uint32_t slot);
std::vector<std::uint32_t> recent_calls(const void* object);
const void* recording_thunk(std::uint32_t slot, const void* target);

inline void init_head(GraphicsHead& head, const void* const* table) {
    head.table = table;
    head.device_bits = 0;
    head.refs.store(1, std::memory_order_relaxed);
}

}  // namespace g6shim::d3d12
