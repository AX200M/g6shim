// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/inflate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "g6shim/shader/error.hpp"

namespace g6shim::shader {

namespace {

constexpr unsigned kMaxCodeBits = 15;
constexpr std::size_t kMaxSymbols = 288;
constexpr std::size_t kFixedLiteralSymbols = 288;
constexpr std::size_t kFixedDistanceSymbols = 30;
constexpr std::size_t kLengthSymbols = 29;
constexpr std::size_t kDistanceSymbols = 30;
constexpr std::size_t kCodeLengthSymbols = 19;
constexpr std::size_t kMaxLiteralSymbols = 286;
constexpr std::size_t kMaxDistanceSymbols = 30;
constexpr std::uint32_t kZlibModulus = 31;
constexpr std::uint32_t kDeflateMethod = 8;
constexpr std::uint32_t kDictionaryFlag = 0x20;
constexpr std::size_t kZlibHeaderSize = 2;

constexpr std::array<std::uint16_t, kLengthSymbols> kLengthBase{
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, kLengthSymbols> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, kDistanceSymbols> kDistanceBase{
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,   33,   49,   65,    97,    129,
    193,  257,  385,  513,  769,  1025,  1537,  2049,  3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, kDistanceSymbols> kDistanceExtra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<std::uint8_t, kCodeLengthSymbols> kCodeLengthOrder{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data, std::size_t at) : data_(data), at_(at) {}

    std::uint32_t bits(unsigned need) {
        while (count_ < need) {
            if (at_ >= data_.size()) detail::fail_malformed("compressed stream is truncated");
            buffer_ |= static_cast<std::uint32_t>(data_[at_++]) << count_;
            count_ += 8;
        }
        const std::uint32_t value = buffer_ & ((std::uint32_t{1} << need) - 1);
        buffer_ >>= need;
        count_ -= need;
        return value;
    }

    void align() {
        buffer_ >>= count_ % 8;
        count_ -= count_ % 8;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t at_ = 0;
    std::uint32_t buffer_ = 0;
    unsigned count_ = 0;
};

struct Huffman {
    std::array<std::uint16_t, kMaxCodeBits + 1> count{};
    std::array<std::uint16_t, kMaxSymbols> symbol{};
};

std::int32_t build(Huffman& table, const std::uint8_t* lengths, std::size_t symbols) {
    table.count.fill(0);
    for (std::size_t i = 0; i < symbols; ++i) ++table.count[lengths[i]];
    if (table.count[0] == symbols) return 0;
    std::int32_t left = 1;
    for (unsigned len = 1; len <= kMaxCodeBits; ++len) {
        left <<= 1;
        left -= table.count[len];
        if (left < 0) return left;
    }
    std::array<std::uint16_t, kMaxCodeBits + 1> offsets{};
    for (unsigned len = 1; len < kMaxCodeBits; ++len)
        offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + table.count[len]);
    for (std::size_t symbol = 0; symbol < symbols; ++symbol)
        if (lengths[symbol] != 0) table.symbol[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
    return left;
}

bool usable(std::int32_t left, const Huffman& table, std::size_t symbols) {
    return left == 0 || (left > 0 && static_cast<std::size_t>(table.count[0] + table.count[1]) == symbols);
}

int decode_symbol(const Huffman& table, BitReader& reader) {
    std::int32_t code = 0;
    std::int32_t first = 0;
    std::int32_t index = 0;
    for (unsigned len = 1; len <= kMaxCodeBits; ++len) {
        code |= static_cast<std::int32_t>(reader.bits(1));
        const std::int32_t count = table.count[len];
        if (code - count < first) {
            const std::int32_t at = index + (code - first);
            if (at < 0 || static_cast<std::size_t>(at) >= table.symbol.size()) return -1;
            return table.symbol[static_cast<std::size_t>(at)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

void emit(std::vector<std::uint8_t>& out, std::size_t limit, std::uint8_t byte) {
    if (out.size() >= limit) detail::fail_malformed("compressed stream is larger than its stated size");
    out.push_back(byte);
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t kModulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kModulus;
        b = (b + a) % kModulus;
    }
    return b << 16 | a;
}

void inflate_codes(std::vector<std::uint8_t>& out,
                   std::size_t limit,
                   BitReader& reader,
                   const Huffman& literals,
                   const Huffman& distances) {
    for (;;) {
        const int symbol = decode_symbol(literals, reader);
        if (symbol < 0) detail::fail_malformed("compressed stream has an invalid code");
        if (symbol < 256) {
            emit(out, limit, static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return;
        const std::size_t length_index = static_cast<std::size_t>(symbol - 257);
        if (length_index >= kLengthSymbols) detail::fail_malformed("compressed stream has an invalid length");
        const std::size_t length = kLengthBase[length_index] + reader.bits(kLengthExtra[length_index]);
        const int distance_symbol = decode_symbol(distances, reader);
        if (distance_symbol < 0 || static_cast<std::size_t>(distance_symbol) >= kDistanceSymbols)
            detail::fail_malformed("compressed stream has an invalid distance");
        const std::size_t distance =
            kDistanceBase[static_cast<std::size_t>(distance_symbol)] +
            reader.bits(kDistanceExtra[static_cast<std::size_t>(distance_symbol)]);
        if (distance == 0 || distance > out.size()) detail::fail_malformed("compressed stream copies from before its start");
        for (std::size_t i = 0; i < length; ++i) emit(out, limit, out[out.size() - distance]);
    }
}

void inflate_fixed(std::vector<std::uint8_t>& out, std::size_t limit, BitReader& reader) {
    std::array<std::uint8_t, kFixedLiteralSymbols> literal_lengths{};
    for (std::size_t i = 0; i < literal_lengths.size(); ++i) {
        if (i <= 143)
            literal_lengths[i] = 8;
        else if (i <= 255)
            literal_lengths[i] = 9;
        else if (i <= 279)
            literal_lengths[i] = 7;
        else
            literal_lengths[i] = 8;
    }
    std::array<std::uint8_t, kFixedDistanceSymbols> distance_lengths{};
    distance_lengths.fill(5);
    Huffman literals;
    Huffman distances;
    if (!usable(build(literals, literal_lengths.data(), literal_lengths.size()), literals, literal_lengths.size()) ||
        !usable(build(distances, distance_lengths.data(), distance_lengths.size()), distances, distance_lengths.size()))
        detail::fail_malformed("compressed stream has an invalid tree");
    inflate_codes(out, limit, reader, literals, distances);
}

void inflate_dynamic(std::vector<std::uint8_t>& out, std::size_t limit, BitReader& reader) {
    const std::size_t literal_count = reader.bits(5) + 257;
    const std::size_t distance_count = reader.bits(5) + 1;
    const std::size_t code_count = reader.bits(4) + 4;
    if (literal_count > kMaxLiteralSymbols || distance_count > kMaxDistanceSymbols)
        detail::fail_malformed("compressed stream has an implausible tree size");
    std::array<std::uint8_t, kMaxSymbols> lengths{};
    for (std::size_t i = 0; i < code_count; ++i) lengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(reader.bits(3));
    Huffman code_table;
    if (build(code_table, lengths.data(), kCodeLengthSymbols) != 0)
        detail::fail_malformed("compressed stream has an invalid tree");
    const std::size_t total = literal_count + distance_count;
    std::size_t index = 0;
    while (index < total) {
        const int symbol = decode_symbol(code_table, reader);
        if (symbol < 0) detail::fail_malformed("compressed stream has an invalid code length");
        if (symbol < 16) {
            lengths[index++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::size_t repeat = 0;
        std::uint8_t value = 0;
        if (symbol == 16) {
            if (index == 0) detail::fail_malformed("compressed stream repeats a missing code length");
            value = lengths[index - 1];
            repeat = 3 + reader.bits(2);
        } else if (symbol == 17) {
            repeat = 3 + reader.bits(3);
        } else {
            repeat = 11 + reader.bits(7);
        }
        if (index + repeat > total) detail::fail_malformed("compressed stream overruns its code lengths");
        for (std::size_t i = 0; i < repeat; ++i) lengths[index++] = value;
    }
    if (lengths[256] == 0) detail::fail_malformed("compressed stream has no end-of-block code");
    Huffman literals;
    Huffman distances;
    if (!usable(build(literals, lengths.data(), literal_count), literals, literal_count))
        detail::fail_malformed("compressed stream has an invalid tree");
    if (!usable(build(distances, lengths.data() + literal_count, distance_count), distances, distance_count))
        detail::fail_malformed("compressed stream has an invalid tree");
    inflate_codes(out, limit, reader, literals, distances);
}

}  // namespace

std::vector<std::uint8_t> inflate_zlib(std::span<const std::uint8_t> stream, std::size_t limit) {
    if (stream.size() < kZlibHeaderSize) detail::fail_malformed("compressed stream is truncated");
    const std::uint32_t cmf = stream[0];
    const std::uint32_t flg = stream[1];
    if ((cmf * 256 + flg) % kZlibModulus != 0) detail::fail_malformed("compressed stream has a bad header check");
    if ((cmf & 0x0F) != kDeflateMethod) detail::fail_malformed("compressed stream is not deflate");
    if ((cmf >> 4) > 7) detail::fail_malformed("compressed stream asks for an oversized window");
    if ((flg & kDictionaryFlag) != 0) detail::fail_malformed("compressed stream needs a dictionary");
    std::vector<std::uint8_t> out;
    BitReader reader(stream, kZlibHeaderSize);
    for (;;) {
        const bool last = reader.bits(1) != 0;
        const std::uint32_t type = reader.bits(2);
        if (type == 0) {
            reader.align();
            const std::uint32_t length = reader.bits(16);
            const std::uint32_t complement = reader.bits(16);
            if ((length ^ 0xFFFF) != complement) detail::fail_malformed("stored block length disagrees with its check");
            for (std::uint32_t i = 0; i < length; ++i) emit(out, limit, static_cast<std::uint8_t>(reader.bits(8)));
        } else if (type == 1) {
            inflate_fixed(out, limit, reader);
        } else if (type == 2) {
            inflate_dynamic(out, limit, reader);
        } else {
            detail::fail_malformed("compressed stream has a reserved block type");
        }
        if (last) break;
    }
    reader.align();
    std::uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored = stored << 8 | reader.bits(8);
    if (stored != adler32(out)) detail::fail_malformed("compressed stream checksum disagrees");
    return out;
}

}  // namespace g6shim::shader
