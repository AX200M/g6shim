// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "alloc.hpp"

#include <cstdint>
#include <cstring>

using namespace g6shim::xmem;

TEST(xmem_allocations_are_aligned_and_usable) {
    CHECK(allocation_alignment(16) == kSmallAlignment);
    CHECK(allocation_alignment(kPageBytes) == kPageBytes);
    void* small = allocate(40);
    void* large = allocate(3 * kPageBytes);
    void* empty = allocate(0);
    CHECK(small && reinterpret_cast<std::uintptr_t>(small) % kSmallAlignment == 0);
    CHECK(large && reinterpret_cast<std::uintptr_t>(large) % kPageBytes == 0);
    CHECK(empty != nullptr);
    if (large) std::memset(large, 0xAB, 3 * kPageBytes);
    release(small);
    release(large);
    release(empty);
    release(nullptr);
}
