// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "apu_memory.hpp"

#include <cstdint>
#include <cstring>

using g6shim::acphal::ApuMemory;
using g6shim::acphal::kApuAddressEnd;

namespace {

std::uintptr_t as_number(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

}  // namespace

TEST(apu_memory_hands_out_zeroed_blocks_below_4_gib) {
    ApuMemory memory;
    void* va = nullptr;
    std::uint32_t apu = 0;
    CHECK(memory.allocate(0x5000, 4, &va, &apu) == S_OK);
    CHECK(va != nullptr);
    CHECK(as_number(va) + 0x5000 <= kApuAddressEnd);
    CHECK(apu == as_number(va));
    const auto* bytes = static_cast<const unsigned char*>(va);
    bool zero = true;
    for (int i = 0; i < 0x5000; ++i) zero = zero && bytes[i] == 0;
    CHECK(zero);
    std::memset(va, 0xAB, 0x5000);
    CHECK(memory.block_count() == 1);
}

TEST(apu_memory_honours_large_alignments) {
    ApuMemory memory;
    void* va = nullptr;
    CHECK(memory.allocate(0x800, 0x40000, &va, nullptr) == S_OK);
    CHECK(va != nullptr);
    CHECK(as_number(va) % 0x40000 == 0);
}

TEST(apu_memory_rejects_bad_requests) {
    ApuMemory memory;
    void* va = reinterpret_cast<void*>(1);
    std::uint32_t apu = 7;
    CHECK(memory.allocate(0, 4, &va, &apu) == E_INVALIDARG);
    CHECK(va == nullptr);
    CHECK(apu == 0);
    CHECK(memory.allocate(0x100, 3, &va, &apu) == E_INVALIDARG);
    CHECK(memory.allocate(0x100, 4, nullptr, &apu) == E_INVALIDARG);
    CHECK(memory.block_count() == 0);
}

TEST(apu_memory_maps_addresses_inside_blocks_only) {
    ApuMemory memory;
    void* va = nullptr;
    std::uint32_t apu = 0;
    CHECK(memory.allocate(0x2000, 0, &va, &apu) == S_OK);
    auto* base = static_cast<unsigned char*>(va);
    CHECK(memory.apu_address(base) == apu);
    CHECK(memory.apu_address(base + 0x1FFF) == apu + 0x1FFF);
    CHECK(memory.apu_address(base + 0x2000) == 0);
    CHECK(memory.apu_address(nullptr) == 0);
    int on_stack = 0;
    CHECK(memory.apu_address(&on_stack) == 0);
    CHECK(memory.virtual_address(apu + 0x10, 0x100) == base + 0x10);
    CHECK(memory.virtual_address(apu + 0x1F00, 0x100) == base + 0x1F00);
    CHECK(memory.virtual_address(apu + 0x1F00, 0x101) == nullptr);
    CHECK(memory.virtual_address(0, 4) == nullptr);
}

TEST(apu_memory_checks_ranges) {
    ApuMemory memory;
    void* va = nullptr;
    CHECK(memory.allocate(0x800, 4, &va, nullptr) == S_OK);
    auto* base = static_cast<unsigned char*>(va);
    CHECK(memory.contains(base, 0x800));
    CHECK(memory.contains(base + 0x7FF, 1));
    CHECK(!memory.contains(base + 0x7FF, 2));
    CHECK(!memory.contains(base - 1, 1));
    CHECK(!memory.contains(nullptr, 1));
}

TEST(apu_memory_frees_only_block_starts) {
    ApuMemory memory;
    void* va = nullptr;
    CHECK(memory.allocate(0x1000, 0, &va, nullptr) == S_OK);
    CHECK(!memory.release(static_cast<unsigned char*>(va) + 8));
    CHECK(memory.release(va));
    CHECK(!memory.release(va));
    CHECK(memory.block_count() == 0);
    CHECK(memory.apu_address(va) == 0);
}

TEST(apu_memory_records_the_heap) {
    ApuMemory memory;
    CHECK(memory.create_heap(0, 0) == E_INVALIDARG);
    CHECK(memory.create_heap(0x100000, 0x400000) == S_OK);
    CHECK(memory.heap().cached == 0x100000);
    CHECK(memory.heap().uncached == 0x400000);
}
