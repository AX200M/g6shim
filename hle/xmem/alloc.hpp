// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g6shim::xmem {

inline constexpr std::size_t kPageBytes = 4096;
inline constexpr std::size_t kSmallAlignment = 64;

std::size_t allocation_alignment(std::size_t bytes);
void* allocate(std::size_t bytes);
void release(void* block);

}  // namespace g6shim::xmem
