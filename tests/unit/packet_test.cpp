// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "packet.hpp"
#include "placement.hpp"

#include <vector>

namespace {

using namespace g6shim::d3d;

std::vector<std::uint8_t> packet(std::uint32_t header, std::size_t entries) {
    std::vector<std::uint8_t> bytes(kPacketHeaderSize + entries * kPacketEntrySize);
    std::memcpy(bytes.data(), &header, sizeof header);
    for (std::size_t i = 0; i < entries; ++i) {
        const auto object = reinterpret_cast<void*>(std::uintptr_t{0x1000} * (i + 1));
        const std::uint64_t value = 0x100 * (i + 1);
        std::memcpy(bytes.data() + kPacketHeaderSize + i * kPacketEntrySize, &object, sizeof object);
        std::memcpy(bytes.data() + kPacketHeaderSize + i * kPacketEntrySize + sizeof object, &value, sizeof value);
    }
    return bytes;
}

std::uint32_t header(std::uint8_t type, std::uint8_t stage, std::uint8_t slot, std::uint8_t count) {
    return std::uint32_t{type} << 24 | std::uint32_t{stage} << 16 | std::uint32_t{slot} << 8 | count;
}

}  // namespace

TEST(packet_parses_header_and_entries) {
    const auto bytes = packet(header(kPacketView, kPixel, 3, 2), 2);
    const auto p = parse_packet(bytes.data(), bytes.data() + bytes.size());
    CHECK(p.has_value());
    if (!p) return;
    CHECK(p->type == kPacketView && p->stage == kPixel && p->first_slot == 3 && p->count == 2);
    CHECK(p->entry(0).object == reinterpret_cast<void*>(0x1000) && p->entry(0).value == 0x100);
    CHECK(p->entry(1).object == reinterpret_cast<void*>(0x2000) && p->entry(1).value == 0x200);
    CHECK(known_stage(*p));
}

TEST(packet_count_is_capped_by_size) {
    const auto bytes = packet(header(kPacketSampler, kVertex, 0, 9), 1);
    const auto p = parse_packet(bytes.data(), bytes.data() + bytes.size());
    CHECK(p && p->count == 1);
    const auto partial = packet(header(kPacketSampler, kVertex, 0, 1), 1);
    const auto q = parse_packet(partial.data(), partial.data() + partial.size() - 1);
    CHECK(q && q->count == 0);
}

TEST(packet_rejects_bad_ranges) {
    const auto bytes = packet(header(kPacketBuffer, kVertex, 0, 1), 1);
    CHECK(!parse_packet(nullptr, bytes.data()));
    CHECK(!parse_packet(bytes.data(), nullptr));
    CHECK(!parse_packet(bytes.data(), bytes.data() + 3));
    CHECK(!parse_packet(bytes.data() + 4, bytes.data()));
    std::vector<std::uint8_t> huge(kMaxPacketSize + 1);
    CHECK(!parse_packet(huge.data(), huge.data() + huge.size()));
}

TEST(packet_stage_rules) {
    const auto views = packet(header(kPacketView, kInputAssembler, 0, 0), 0);
    CHECK(!known_stage(*parse_packet(views.data(), views.data() + views.size())));
    const auto vertex = packet(header(kPacketBuffer, kInputAssembler, 0, 0), 0);
    CHECK(known_stage(*parse_packet(vertex.data(), vertex.data() + vertex.size())));
    const auto beyond = packet(header(kPacketBuffer, 7, 0, 0), 0);
    CHECK(!known_stage(*parse_packet(beyond.data(), beyond.data() + beyond.size())));
}

TEST(packet_buffer_values) {
    CHECK(buffer_stride(0x0020000000001234ull) == 0x20);
    BufferTarget plain;
    CHECK(resolve_buffer(0x0020000000001234ull, 0, nullptr, plain));
    CHECK(plain.offset == 0x1234 && plain.memory == nullptr);
}

TEST(packet_buffers_in_placement_memory) {
    std::vector<std::uint8_t> memory(0x400);
    const Placement placement{memory.data(), static_cast<std::uint32_t>(memory.size())};
    const auto start = reinterpret_cast<std::uint64_t>(memory.data());
    const std::uint64_t address_field = 0x10;

    BufferTarget inside;
    CHECK(resolve_buffer((start + 0x100 - address_field) & kLow48, address_field, &placement, inside));
    CHECK(inside.offset == 0x100 && inside.memory == memory.data() + 0x100 && inside.available == 0x300);

    BufferTarget past;
    CHECK(!resolve_buffer((start + 0x400 - address_field) & kLow48, address_field, &placement, past));
    BufferTarget before;
    CHECK(!resolve_buffer((start - 1 - address_field) & kLow48, address_field, &placement, before));
}
