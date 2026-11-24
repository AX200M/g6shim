// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <cstdint>

namespace g6shim::d3d12_xs::frame {

inline constexpr std::uint32_t kOrigin = 0;
inline constexpr std::uint32_t kTokenOnTimeout = 2;

HRESULT set_interval(Object* device, Object* output, std::uint32_t length_microseconds, std::uint32_t period,
                     std::uint32_t flags);
HRESULT schedule(Object* device, std::uint32_t kind, std::uint32_t offset, const std::uint8_t* list,
                 std::uint32_t flags);
HRESULT wait(Object* device, std::uint32_t kind, std::uint32_t timeout_ms, const std::uint8_t* list,
             std::uint32_t flags, std::uint64_t* token);
HRESULT statistics(Object* device, std::uint64_t token, std::uint32_t kinds, std::uint32_t* count, void* out);

std::uint64_t current_frame();
std::uint64_t last_tick(std::uint32_t kind);
std::uint64_t next_tick_number(std::uint32_t kind);

}  // namespace g6shim::d3d12_xs::frame
