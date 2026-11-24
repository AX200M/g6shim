// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::d3d {

inline constexpr std::uint64_t kLow48 = (1ull << 48) - 1;
inline constexpr std::size_t kBufferAddressField = 0x30;

struct Placement {
    const std::uint8_t* address = nullptr;
    std::uint32_t size = 0;
};

struct BufferTarget {
    std::uint64_t offset = 0;
    const std::uint8_t* memory = nullptr;
    std::uint32_t available = 0;
};

inline bool resolve_buffer(std::uint64_t value, std::uint64_t address_field, const Placement* placement,
                           BufferTarget& t) {
    if (!placement) {
        t.offset = value & kLow48;
        return true;
    }
    const std::uint64_t address = ((value & kLow48) + address_field) & kLow48;
    const auto start = reinterpret_cast<std::uint64_t>(placement->address);
    if (address < start || address >= start + placement->size) return false;
    t.offset = address - start;
    t.memory = placement->address + t.offset;
    t.available = static_cast<std::uint32_t>(placement->size - t.offset);
    return true;
}

HRESULT STDMETHODCALLTYPE CreatePlacementBuffer(void* self, const D3D11_BUFFER_DESC* desc, void* address,
                                                ID3D11Buffer** buffer);

bool placement_of(const void* buffer, Placement& out);

void forget_placement(const void* buffer);

}  // namespace g6shim::d3d
