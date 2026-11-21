// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace g6shim::d3d {

inline constexpr std::size_t kPacketHeaderSize = 4;
inline constexpr std::size_t kPacketEntrySize = 16;
inline constexpr std::size_t kMaxPacketSize = 4096;
inline constexpr unsigned kStrideShift = 48;

enum PacketType : std::uint8_t { kPacketView = 0x80, kPacketBuffer = 0x91, kPacketSampler = 0xA0 };

enum Stage : unsigned { kVertex, kHull, kDomain, kGeometry, kPixel, kCompute, kInputAssembler };
inline constexpr unsigned kShaderStages = kInputAssembler;

struct PacketEntry {
    void* object = nullptr;
    std::uint64_t value = 0;
};

struct Packet {
    std::uint32_t header = 0;
    std::uint8_t type = 0;
    unsigned stage = 0;
    unsigned first_slot = 0;
    std::size_t count = 0;
    const std::uint8_t* entries = nullptr;

    PacketEntry entry(std::size_t i) const {
        PacketEntry e;
        std::memcpy(&e.object, entries + kPacketEntrySize * i, sizeof e.object);
        std::memcpy(&e.value, entries + kPacketEntrySize * i + sizeof(void*), sizeof e.value);
        return e;
    }
};

inline std::optional<Packet> parse_packet(const std::uint8_t* begin, const std::uint8_t* end) {
    if (!begin || !end || end < begin + kPacketHeaderSize) return std::nullopt;
    const auto size = static_cast<std::size_t>(end - begin);
    if (size > kMaxPacketSize) return std::nullopt;
    Packet p;
    std::memcpy(&p.header, begin, sizeof p.header);
    p.type = static_cast<std::uint8_t>(p.header >> 24);
    p.stage = (p.header >> 16) & 0xFF;
    p.first_slot = (p.header >> 8) & 0xFF;
    const std::size_t declared = p.header & 0xFF;
    const std::size_t fits = (size - kPacketHeaderSize) / kPacketEntrySize;
    p.count = declared < fits ? declared : fits;
    p.entries = begin + kPacketHeaderSize;
    return p;
}

inline bool known_stage(const Packet& p) {
    if (p.stage > kInputAssembler) return false;
    return p.stage != kInputAssembler || p.type == kPacketBuffer;
}

inline std::uint32_t buffer_stride(std::uint64_t value) { return static_cast<std::uint32_t>(value >> kStrideShift); }

}  // namespace g6shim::d3d
