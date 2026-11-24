// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "alloc.hpp"

#include <malloc.h>

namespace g6shim::xmem {

std::size_t allocation_alignment(std::size_t bytes) {
    return bytes >= kPageBytes ? kPageBytes : kSmallAlignment;
}

void* allocate(std::size_t bytes) {
    const std::size_t size = bytes ? bytes : 1;
    return _aligned_malloc(size, allocation_alignment(size));
}

void release(void* block) { _aligned_free(block); }

}  // namespace g6shim::xmem

extern "C" void* g6shim_XMemAlloc(std::size_t bytes, std::uint64_t) { return g6shim::xmem::allocate(bytes); }

extern "C" void g6shim_XMemFree(void* block, std::uint64_t) { g6shim::xmem::release(block); }
