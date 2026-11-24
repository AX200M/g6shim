// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/inflate.hpp"
#include "g6shim/shader/scan.hpp"

namespace {

using namespace g6shim::shader;

constexpr std::uint32_t kFuzzSeed = 0x5A1D'0F00u;
constexpr std::size_t kFuzzInputLimit = 1024;
constexpr std::size_t kFuzzOutputLimit = 1 << 16;
constexpr std::size_t kMaxStoredBlock = 65535;
constexpr std::array<std::uint8_t, 4> kRecordMarker{0xE7, 0xDC, 0x0D, 0x00};
constexpr std::array<std::uint8_t, 4> kContainerMagic{'D', 'X', 'B', 'C'};
constexpr std::array<std::uint8_t, 19> kCodeLengthOrder{16, 17, 18, 0, 8, 7, 9, 6, 10,
                                                        5,  11, 4,  12, 3, 13, 2, 14, 1, 15};
constexpr std::size_t kCodeLengthAlphabet = 19;
constexpr std::size_t kMaxLiteralCount = 286;
constexpr std::size_t kMaxDistanceCount = 30;
constexpr std::uint32_t kFixedCodeBits = 3;

class BitWriter {
public:
    void bit(unsigned value) {
        current_ = static_cast<std::uint8_t>(current_ | ((value & 1) << count_));
        if (++count_ == 8) {
            bytes_.push_back(current_);
            current_ = 0;
            count_ = 0;
        }
    }

    void code(std::uint32_t value, unsigned bits) {
        for (unsigned index = bits; index != 0; --index) bit((value >> (index - 1)) & 1);
    }

    void value(std::uint32_t number, unsigned bits) {
        for (unsigned index = 0; index != bits; ++index) bit((number >> index) & 1);
    }

    std::vector<std::uint8_t> done() {
        if (count_ != 0) bytes_.push_back(current_);
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint8_t current_ = 0;
    unsigned count_ = 0;
};

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kModulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kModulus;
        b = (b + a) % kModulus;
    }
    return b << 16 | a;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_stored(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> data, bool final) {
    const std::size_t length = std::min(data.size(), kMaxStoredBlock);
    out.push_back(static_cast<std::uint8_t>(final ? 1 : 0));
    out.push_back(static_cast<std::uint8_t>(length));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
    out.push_back(static_cast<std::uint8_t>(inverse));
    out.push_back(static_cast<std::uint8_t>(inverse >> 8));
    out.insert(out.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
}

std::vector<std::uint8_t> raw_stored(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    do {
        const std::size_t length = std::min(kMaxStoredBlock, data.size() - at);
        append_stored(out, data.subspan(at, length), at + length == data.size());
        at += length;
    } while (at < data.size());
    return out;
}

std::vector<std::uint8_t> zlib_stored(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    const std::vector<std::uint8_t> raw = raw_stored(data);
    out.insert(out.end(), raw.begin(), raw.end());
    const std::uint32_t check = adler32(data);
    out.push_back(static_cast<std::uint8_t>(check >> 24));
    out.push_back(static_cast<std::uint8_t>(check >> 16));
    out.push_back(static_cast<std::uint8_t>(check >> 8));
    out.push_back(static_cast<std::uint8_t>(check));
    return out;
}

std::vector<std::uint8_t> fixed_literals(std::span<const std::uint8_t> data) {
    BitWriter writer;
    writer.bit(1);
    writer.value(1, 2);
    for (const std::uint8_t byte : data) {
        if (byte <= 143)
            writer.code(0x30u + byte, 8);
        else
            writer.code(0x190u + (byte - 144), 9);
    }
    writer.code(0, 7);
    return writer.done();
}

std::vector<std::uint8_t> random_bytes(std::mt19937& rng, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    for (std::uint8_t& byte : out) byte = static_cast<std::uint8_t>(rng());
    return out;
}

void place(std::vector<std::uint8_t>& data, std::span<const std::uint8_t> magic, std::size_t at) {
    if (at > data.size() || magic.size() > data.size() - at) return;
    std::copy(magic.begin(), magic.end(), data.begin() + static_cast<std::ptrdiff_t>(at));
}

void mutate(std::mt19937& rng, std::vector<std::uint8_t>& data) {
    const auto edit = [&] {
        if (data.empty()) return;
        const std::size_t at = rng() % data.size();
        switch (rng() % 3) {
            case 0: data[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
            case 1: data[at] = static_cast<std::uint8_t>(rng()); break;
            default: data.resize(at); break;
        }
    };
    const unsigned edits = 1 + rng() % 6;
    for (unsigned index = 0; index < edits; ++index) edit();
    if (rng() % 4 == 0) {
        const std::vector<std::uint8_t> tail = random_bytes(rng, rng() % 32);
        data.insert(data.end(), tail.begin(), tail.end());
    }
}

void inflate_quietly(std::span<const std::uint8_t> stream) {
    try {
        (void)inflate_raw(stream, kFuzzOutputLimit);
    } catch (const ParseError&) {
    }
    try {
        (void)inflate_zlib(stream, kFuzzOutputLimit);
    } catch (const ParseError&) {
    }
}

void unpack_quietly(const PackedShaderRecord& record) {
    try {
        (void)record.unpack();
    } catch (const ParseError&) {
    }
}

std::array<std::uint32_t, kCodeLengthAlphabet> canonical_codes(
    const std::array<std::uint8_t, kCodeLengthAlphabet>& lengths) {
    std::array<std::uint32_t, 16> counts{};
    for (const std::uint8_t length : lengths) ++counts[length];
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (unsigned bits = 1; bits <= 15; ++bits) {
        code = (code + counts[bits - 1]) << 1;
        next[bits] = code;
    }
    std::array<std::uint32_t, kCodeLengthAlphabet> codes{};
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol] != 0) codes[symbol] = next[lengths[symbol]]++;
    return codes;
}

constexpr std::array<std::uint8_t, kCodeLengthAlphabet> kFixedCodeLengths{
    3, 3, 3, 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3};

std::array<std::uint8_t, kCodeLengthAlphabet> order_lengths(
    const std::array<std::uint8_t, kCodeLengthAlphabet>& lengths) {
    std::array<std::uint8_t, kCodeLengthAlphabet> ordered{};
    for (std::size_t index = 0; index < kCodeLengthOrder.size(); ++index)
        ordered[index] = lengths[kCodeLengthOrder[index]];
    return ordered;
}

std::vector<std::uint8_t> dynamic_header(std::mt19937& rng) {
    BitWriter writer;
    writer.bit(1);
    writer.value(2, 2);
    const std::size_t literal_count = 257 + rng() % (kMaxLiteralCount - 257 + 1);
    const std::size_t distance_count = 1 + rng() % kMaxDistanceCount;
    writer.value(static_cast<std::uint32_t>(literal_count - 257), 5);
    writer.value(static_cast<std::uint32_t>(distance_count - 1), 5);
    writer.value(15, 4);
    const auto ordered = order_lengths(kFixedCodeLengths);
    for (const std::uint8_t length : ordered) writer.value(length, kFixedCodeBits);
    const auto codes = canonical_codes(kFixedCodeLengths);
    const unsigned repeats = static_cast<unsigned>(rng() % (literal_count + distance_count + 32));
    for (unsigned index = 0; index < repeats; ++index) {
        std::array<std::size_t, 8> alphabet{0, 1, 2, 3, 8, 16, 17, 18};
        const std::size_t symbol = alphabet[rng() % alphabet.size()];
        writer.code(codes[symbol], kFixedCodeBits);
        if (symbol == 16)
            writer.value(static_cast<std::uint32_t>(rng() % 4), 2);
        else if (symbol == 17)
            writer.value(static_cast<std::uint32_t>(rng() % 8), 3);
        else if (symbol == 18)
            writer.value(static_cast<std::uint32_t>(rng() % 128), 7);
    }
    const std::vector<std::uint8_t> tail = random_bytes(rng, rng() % 64);
    std::vector<std::uint8_t> out = writer.done();
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

void write_repeats(BitWriter& writer, std::uint32_t code, unsigned bits, std::size_t total) {
    while (total != 0) {
        const std::size_t repeat = std::min<std::size_t>(total, 138);
        writer.code(code, bits);
        writer.value(static_cast<std::uint32_t>(repeat - 11), 7);
        total -= repeat;
    }
}

std::vector<std::uint8_t> wide_tree_stream() {
    constexpr std::array<std::uint8_t, kCodeLengthAlphabet> kLengths{
        0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    BitWriter writer;
    writer.bit(1);
    writer.value(2, 2);
    writer.value(static_cast<std::uint32_t>(kMaxLiteralCount - 257), 5);
    writer.value(static_cast<std::uint32_t>(kMaxDistanceCount - 1), 5);
    writer.value(15, 4);
    const auto ordered = order_lengths(kLengths);
    for (const std::uint8_t length : ordered) writer.value(length, kFixedCodeBits);
    const auto codes = canonical_codes(kLengths);
    write_repeats(writer, codes[18], 1, 256);
    writer.code(codes[1], 1);
    write_repeats(writer, codes[18], 1, kMaxLiteralCount + kMaxDistanceCount - 257);
    writer.code(0, 1);
    return writer.done();
}

std::vector<std::uint8_t> packed_record(const std::string& name, std::uint8_t kind,
                                        std::span<const std::uint8_t> payload) {
    const std::vector<std::uint8_t> packed = fixed_literals(payload);
    const std::uint32_t total = static_cast<std::uint32_t>(packed.size()) + 12;
    std::vector<std::uint8_t> out{0xF4, 0x01, 0x00, 0x00, static_cast<std::uint8_t>(name.size() + 1)};
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    out.push_back(kind);
    append_u32(out, total);
    out.insert(out.end(), kRecordMarker.begin(), kRecordMarker.end());
    append_u32(out, total);
    append_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

std::vector<std::uint8_t> dxbc_container(std::span<const std::uint8_t> body) {
    constexpr std::size_t kHeader = 0x20;
    constexpr std::size_t kPart = 8;
    constexpr std::size_t kTable = kHeader + 4;
    std::vector<std::uint8_t> out{'D', 'X', 'B', 'C'};
    out.resize(20, 0);
    append_u32(out, 1);
    const std::uint32_t total = static_cast<std::uint32_t>(kTable + kPart + body.size());
    append_u32(out, total);
    append_u32(out, 1);
    append_u32(out, static_cast<std::uint32_t>(kTable));
    out.insert(out.end(), {'S', 'F', 'I', '0'});
    append_u32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST(shader_fuzz_random_inflate) {
    std::mt19937 rng(kFuzzSeed);
    for (int round = 0; round < 3000; ++round) {
        const std::vector<std::uint8_t> buffer = random_bytes(rng, rng() % (kFuzzInputLimit + 1));
        inflate_quietly(buffer);
    }
    CHECK(true);
}

TEST(shader_fuzz_dynamic_headers) {
    std::mt19937 rng(kFuzzSeed ^ 0x9E37u);
    for (int round = 0; round < 2000; ++round) {
        const std::vector<std::uint8_t> buffer = dynamic_header(rng);
        inflate_quietly(buffer);
    }
    CHECK(true);
}

TEST(shader_fuzz_mutated_streams) {
    std::vector<std::uint8_t> payload(512);
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<std::uint8_t>(index * 13 + 7);
    const std::vector<std::uint8_t> fixed = fixed_literals(payload);
    const std::vector<std::uint8_t> stored = raw_stored(payload);
    const std::vector<std::uint8_t> wrapped = zlib_stored(payload);
    std::mt19937 rng(kFuzzSeed ^ 0x1234u);
    for (const std::vector<std::uint8_t>* stream : {&fixed, &stored, &wrapped}) {
        for (int round = 0; round < 750; ++round) {
            std::vector<std::uint8_t> buffer = *stream;
            mutate(rng, buffer);
            inflate_quietly(buffer);
        }
    }
    CHECK(inflate_raw(stored, payload.size()) == payload);
    CHECK(inflate_zlib(wrapped, payload.size()) == payload);
    CHECK(inflate_raw(fixed, payload.size()) == payload);
}

TEST(shader_fuzz_wide_trees) {
    const std::vector<std::uint8_t> buffer = wide_tree_stream();
    CHECK(inflate_raw(buffer, kFuzzOutputLimit).empty());
    std::mt19937 rng(kFuzzSeed ^ 0x7777u);
    for (int round = 0; round < 500; ++round) {
        std::vector<std::uint8_t> mutated = buffer;
        mutate(rng, mutated);
        inflate_quietly(mutated);
    }
}

TEST(shader_fuzz_record_markers) {
    std::mt19937 rng(kFuzzSeed ^ 0xBEEFu);
    std::size_t records = 0;
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::uint8_t> buffer = random_bytes(rng, 32 + rng() % 900);
        const std::size_t at = 6 + rng() % (buffer.size() - 22);
        place(buffer, kRecordMarker, at);
        const std::uint32_t size = static_cast<std::uint32_t>(12 + rng() % 64);
        for (unsigned index = 0; index < 4; ++index) {
            buffer[at - 4 + index] = static_cast<std::uint8_t>(size >> (8 * index));
            buffer[at + 4 + index] = static_cast<std::uint8_t>(size >> (8 * index));
        }
        const std::uint32_t stated = static_cast<std::uint32_t>(rng() % 4096);
        for (unsigned index = 0; index < 4; ++index)
            buffer[at + 8 + index] = static_cast<std::uint8_t>(stated >> (8 * index));
        if (rng() % 3 == 0) place(buffer, kContainerMagic, rng() % (buffer.size() - 4));
        for (const PackedShaderRecord& record : packed_shader_records(buffer)) {
            ++records;
            unpack_quietly(record);
        }
        (void)stored_shader_containers(buffer);
    }
    CHECK(records > 0);
}

TEST(shader_fuzz_mutated_records) {
    const std::vector<std::uint8_t> body{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> container = dxbc_container(body);
    const std::vector<std::uint8_t> record = packed_record("VS_Fuzz_0011AABB", 0x32, container);
    std::vector<std::uint8_t> file(7, 0x5A);
    constexpr std::size_t kRecordName = 16;
    constexpr std::size_t kRecordMarkerAt = 7 + 7 + kRecordName;
    file.insert(file.end(), record.begin(), record.end());
    const std::vector<PackedShaderRecord> found = packed_shader_records(file);
    CHECK(found.size() == 1);
    if (found.size() == 1) CHECK(found[0].unpack() == container);

    std::mt19937 rng(kFuzzSeed ^ 0xC0DEu);
    for (int round = 0; round < 750; ++round) {
        std::vector<std::uint8_t> buffer = file;
        mutate(rng, buffer);
        place(buffer, kRecordMarker, kRecordMarkerAt);
        for (const PackedShaderRecord& item : packed_shader_records(buffer)) unpack_quietly(item);
        (void)stored_shader_containers(buffer);
    }
}
