// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::xmem {

inline constexpr std::uint64_t kGraphicsFlags = 3;
inline constexpr std::uint64_t kAddress32BitFlags = 4;
inline constexpr std::uint64_t kAddressFieldShift = 12;
inline constexpr std::uint64_t kAddressFieldMask = 7;
inline constexpr std::size_t kLowestTitleAddress = std::size_t{1} << 32;
inline constexpr unsigned kDefaultAddressBits = 40;
inline constexpr unsigned kAddressBitsBase = 47;
inline constexpr std::uint32_t kLargePages2M = 0x20000000;
inline constexpr std::uint32_t kPages64KBit = 0x400000;
inline constexpr std::uint32_t kPages64K = kLargePages2M | kPages64KBit;
inline constexpr std::uint32_t kHostProtections = 0x1ff;
inline constexpr std::size_t kLargePageBytes = std::size_t{2} << 20;

struct AddressRange {
    std::size_t lowest;
    std::size_t highest;
};

AddressRange address_range(std::uint64_t flags);

struct BasicInformation {
    MEMORY_BASIC_INFORMATION host;
    std::uint64_t flags;
    std::uint32_t tag_code;
    std::uint8_t tag[4];
};
static_assert(sizeof(BasicInformation) == 0x40);

void* allocate_virtual(void* base, std::size_t bytes, std::uint32_t type, std::uint64_t flags,
                       std::uint32_t protect);

}  // namespace g6shim::xmem
