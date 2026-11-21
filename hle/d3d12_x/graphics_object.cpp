// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "graphics_object.hpp"

#include <intrin.h>

#include <g6shim/log.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace g6shim::d3d12 {

namespace {

constexpr std::size_t kThunkBytes = 16;
constexpr std::uint8_t kLoadPcObject[] = {0x48, 0x8B, 0x89};
constexpr std::uint8_t kLoadPcTable[] = {0x48, 0x8B, 0x01};
constexpr std::uint8_t kJumpThroughPcTable[] = {0xFF, 0xA0};
constexpr int kLoggedProbeCalls = 4;

struct Registry {
    std::mutex mutex;
    std::map<std::size_t, const std::uint8_t*> thunks;
    std::map<const void* const*, const char*> kinds;
    std::vector<std::unique_ptr<std::vector<const void*>>> tables;
};

Registry& registry() {
    static Registry r;
    return r;
}

const std::uint8_t* make_thunks(std::size_t pc_object_offset) {
    const std::size_t bytes = kMaxTableSlots * kThunkBytes;
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code) return nullptr;
    const auto offset = static_cast<std::uint32_t>(pc_object_offset);
    for (std::size_t slot = 0; slot < kMaxTableSlots; ++slot) {
        std::uint8_t* p = code + slot * kThunkBytes;
        const auto displacement = static_cast<std::uint32_t>(slot * sizeof(void*));
        std::memcpy(p, kLoadPcObject, sizeof kLoadPcObject);
        std::memcpy(p + 3, &offset, sizeof offset);
        std::memcpy(p + 7, kLoadPcTable, sizeof kLoadPcTable);
        std::memcpy(p + 10, kJumpThroughPcTable, sizeof kJumpThroughPcTable);
        std::memcpy(p + 12, &displacement, sizeof displacement);
    }
    DWORD old = 0;
    VirtualProtect(code, bytes, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, bytes);
    return code;
}

template <std::size_t Slot>
HRESULT STDMETHODCALLTYPE probe(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < kLoggedProbeCalls)
        G6SHIM_WARN("%s: console slot %zu not implemented, called from %p, args 0x%llX 0x%llX 0x%llX 0x%llX -> E_NOTIMPL",
                    table_kind(self), Slot, _ReturnAddress(), static_cast<unsigned long long>(a),
                    static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
                    static_cast<unsigned long long>(d));
    return E_NOTIMPL;
}

template <std::size_t... I>
constexpr std::array<const void*, kMaxTableSlots> probe_table(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&probe<I>)...};
}

}  // namespace

const void* forward_thunk(std::size_t pc_object_offset, std::size_t slot) {
    if (slot >= kMaxTableSlots || pc_object_offset > kMaxPcObjectOffset) return nullptr;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto& code = r.thunks[pc_object_offset];
    if (!code) code = make_thunks(pc_object_offset);
    return code ? code + slot * kThunkBytes : nullptr;
}

const void* const* build_table(const TableSpec& spec, std::initializer_list<SlotOverride> overrides) {
    return build_table(spec, std::vector<SlotOverride>(overrides));
}

const void* const* build_table(const TableSpec& spec, const std::vector<SlotOverride>& overrides) {
    if (spec.slots > kMaxTableSlots || spec.pc_slots > spec.slots) return nullptr;
    static const auto probes = probe_table(std::make_index_sequence<kMaxTableSlots>{});
    auto table = std::make_unique<std::vector<const void*>>(spec.slots);
    for (std::size_t slot = 0; slot < spec.slots; ++slot) {
        const void* forward = slot < spec.pc_slots ? forward_thunk(spec.pc_object_offset, slot) : nullptr;
        (*table)[slot] = forward ? forward : probes[slot];
    }
    for (const auto& [slot, fn] : overrides)
        if (slot < spec.slots) (*table)[slot] = fn;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const void* const* data = table->data();
    r.kinds[data] = spec.kind;
    r.tables.push_back(std::move(table));
    return data;
}

const char* table_kind(const void* object) {
    if (!object) return "null";
    const auto* table = *static_cast<const void* const* const*>(object);
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.kinds.find(table);
    return it == r.kinds.end() ? "unknown object" : it->second;
}

}  // namespace g6shim::d3d12
