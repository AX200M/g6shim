// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
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
constexpr std::size_t kRecordingThunkBytes = 128;

constexpr std::uint8_t kSaveArguments[] = {
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x4C, 0x24, 0x20,
    0x48, 0x89, 0x54, 0x24, 0x28,
    0x4C, 0x89, 0x44, 0x24, 0x30,
    0x4C, 0x89, 0x4C, 0x24, 0x38,
    0x0F, 0x29, 0x44, 0x24, 0x40,
    0x0F, 0x29, 0x4C, 0x24, 0x50,
    0x0F, 0x29, 0x54, 0x24, 0x60,
    0x0F, 0x29, 0x5C, 0x24, 0x70};
constexpr std::uint8_t kLoadSlot = 0xBA;
constexpr std::uint8_t kLoadRax[] = {0x48, 0xB8};
constexpr std::uint8_t kCallRax[] = {0xFF, 0xD0};
constexpr std::uint8_t kRestoreArguments[] = {
    0x48, 0x8B, 0x4C, 0x24, 0x20,
    0x48, 0x8B, 0x54, 0x24, 0x28,
    0x4C, 0x8B, 0x44, 0x24, 0x30,
    0x4C, 0x8B, 0x4C, 0x24, 0x38,
    0x0F, 0x28, 0x44, 0x24, 0x40,
    0x0F, 0x28, 0x4C, 0x24, 0x50,
    0x0F, 0x28, 0x54, 0x24, 0x60,
    0x0F, 0x28, 0x5C, 0x24, 0x70,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00};
constexpr std::uint8_t kJumpRax[] = {0xFF, 0xE0};

struct CallRing {
    std::array<std::uint32_t, kRecordedCalls> slots{};
    std::size_t next = 0;
};

struct Recorder {
    std::mutex mutex;
    std::map<const void*, CallRing> rings;
};

Recorder& recorder() {
    static Recorder* r = new Recorder;
    return *r;
}

std::uint8_t* put(std::uint8_t* p, const void* bytes, std::size_t count) {
    std::memcpy(p, bytes, count);
    return p + count;
}

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

void record_call(const void* object, std::uint32_t slot) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.mutex);
    CallRing& ring = r.rings[object];
    ring.slots[ring.next % kRecordedCalls] = slot;
    ++ring.next;
}

std::vector<std::uint32_t> recent_calls(const void* object) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.rings.find(object);
    if (it == r.rings.end()) return {};
    const CallRing& ring = it->second;
    const std::size_t count = ring.next < kRecordedCalls ? ring.next : kRecordedCalls;
    std::vector<std::uint32_t> calls;
    for (std::size_t i = ring.next - count; i < ring.next; ++i) calls.push_back(ring.slots[i % kRecordedCalls]);
    return calls;
}

const void* recording_thunk(std::uint32_t slot, const void* target) {
    auto* code = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kRecordingThunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code) return target;
    const auto recorder_address = reinterpret_cast<std::uintptr_t>(&record_call);
    const auto target_address = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t* p = put(code, kSaveArguments, sizeof kSaveArguments);
    *p++ = kLoadSlot;
    p = put(p, &slot, sizeof slot);
    p = put(p, kLoadRax, sizeof kLoadRax);
    p = put(p, &recorder_address, sizeof recorder_address);
    p = put(p, kCallRax, sizeof kCallRax);
    p = put(p, kRestoreArguments, sizeof kRestoreArguments);
    p = put(p, kLoadRax, sizeof kLoadRax);
    p = put(p, &target_address, sizeof target_address);
    put(p, kJumpRax, sizeof kJumpRax);
    DWORD old = 0;
    VirtualProtect(code, kRecordingThunkBytes, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, kRecordingThunkBytes);
    return code;
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
    if (trace_enabled())
        for (std::size_t slot = 0; slot < spec.slots; ++slot)
            (*table)[slot] = recording_thunk(static_cast<std::uint32_t>(slot), (*table)[slot]);
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
