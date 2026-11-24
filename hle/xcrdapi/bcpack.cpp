// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bcpack.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace g6shim::xcrdapi {

namespace {

constexpr std::array<std::uint8_t, 4> kBc1Code{0, 2, 3, 1};
constexpr std::array<std::uint64_t, 8> kBc4Code{0, 2, 3, 4, 5, 6, 7, 1};

constexpr std::array<std::uint8_t, 64> kAnchor2{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8, 15, 15, 2, 8, 2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2, 2, 15};
constexpr std::array<std::uint8_t, 64> kAnchor3A{
    3,  3,  15, 15, 8,  3,  15, 15, 8, 8, 6,  6, 6,  5,  3,  3,  3, 3, 8, 15, 3, 3,
    6,  10, 5,  8,  8,  6,  8,  5,  15, 15, 8, 15, 3, 5,  6,  10, 8, 15, 15, 3, 15, 5,
    15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
constexpr std::array<std::uint8_t, 64> kAnchor3B{
    15, 8, 8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3, 15, 8,
    15, 8, 3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

constexpr std::array<std::array<std::uint8_t, 4>, 8> kBc7Bits{{
    {4, 4, 4, 0},
    {6, 6, 6, 0},
    {5, 5, 5, 0},
    {7, 7, 7, 0},
    {5, 5, 5, 6},
    {7, 7, 7, 8},
    {7, 7, 7, 7},
    {5, 5, 5, 5},
}};

constexpr std::array<std::array<std::uint8_t, 2>, 8> kBc7Index{{
    {3, 0},
    {3, 0},
    {2, 0},
    {2, 0},
    {3, 2},
    {2, 2},
    {4, 0},
    {2, 0},
}};

constexpr std::array<std::array<std::uint8_t, 4>, 15> kBc6Bits{{
    {},
    {10, 5, 5, 5},
    {7, 6, 6, 6},
    {11, 5, 4, 4},
    {11, 4, 5, 4},
    {11, 4, 4, 5},
    {9, 5, 5, 5},
    {8, 6, 5, 5},
    {8, 5, 6, 5},
    {8, 5, 5, 6},
    {6, 6, 6, 6},
    {10, 10, 10, 10},
    {11, 9, 9, 9},
    {12, 8, 8, 8},
    {16, 4, 4, 4},
}};

constexpr std::array<std::pair<std::uint64_t, std::uint32_t>, 15> kBc6Code{{
    {0, 0},  {0, 2},  {1, 2},  {2, 5},  {6, 5},  {10, 5}, {14, 5}, {18, 5},
    {22, 5}, {26, 5}, {30, 5}, {3, 5},  {7, 5},  {11, 5}, {15, 5},
}};

struct Run {
    std::uint8_t ch;
    std::uint8_t ep;
    std::uint8_t first;
    std::uint8_t len;
};

constexpr Run rt(std::uint8_t ch, std::uint8_t ep, std::uint8_t first, std::uint8_t len) {
    return Run{ch, ep, first, len};
}

constexpr std::uint8_t kR = 0;
constexpr std::uint8_t kG = 1;
constexpr std::uint8_t kB = 2;
constexpr Run kD = rt(3, 0, 0, 5);

constexpr Run kLayout1[] = {
    rt(kG, 2, 4, 1), rt(kB, 2, 4, 1), rt(kB, 3, 4, 1), rt(kR, 0, 0, 10), rt(kG, 0, 0, 10),
    rt(kB, 0, 0, 10), rt(kR, 1, 0, 5), rt(kG, 3, 4, 1), rt(kG, 2, 0, 4),  rt(kG, 1, 0, 5),
    rt(kB, 3, 0, 1),  rt(kG, 3, 0, 4), rt(kB, 1, 0, 5), rt(kB, 3, 1, 1),  rt(kB, 2, 0, 4),
    rt(kR, 2, 0, 5),  rt(kB, 3, 2, 1), rt(kR, 3, 0, 5), rt(kB, 3, 3, 1),  kD,
};
constexpr Run kLayout2[] = {
    rt(kG, 2, 5, 1), rt(kG, 3, 4, 1), rt(kG, 3, 5, 1), rt(kR, 0, 0, 7), rt(kB, 3, 0, 1),
    rt(kB, 3, 1, 1), rt(kB, 2, 4, 1), rt(kG, 0, 0, 7), rt(kB, 2, 5, 1), rt(kB, 3, 2, 1),
    rt(kG, 2, 4, 1), rt(kB, 0, 0, 7), rt(kB, 3, 3, 1), rt(kB, 3, 5, 1), rt(kB, 3, 4, 1),
    rt(kR, 1, 0, 6), rt(kG, 2, 0, 4), rt(kG, 1, 0, 6), rt(kG, 3, 0, 4), rt(kB, 1, 0, 6),
    rt(kB, 2, 0, 4), rt(kR, 2, 0, 6), rt(kR, 3, 0, 6), kD,
};
constexpr Run kLayout3[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 5), rt(kR, 0, 10, 1),
    rt(kG, 2, 0, 4),  rt(kG, 1, 0, 4),  rt(kG, 0, 10, 1), rt(kB, 3, 0, 1), rt(kG, 3, 0, 4),
    rt(kB, 1, 0, 4),  rt(kB, 0, 10, 1), rt(kB, 3, 1, 1),  rt(kB, 2, 0, 4), rt(kR, 2, 0, 5),
    rt(kB, 3, 2, 1),  rt(kR, 3, 0, 5),  rt(kB, 3, 3, 1),  kD,
};
constexpr Run kLayout4[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 4), rt(kR, 0, 10, 1),
    rt(kG, 3, 4, 1),  rt(kG, 2, 0, 4),  rt(kG, 1, 0, 5),  rt(kG, 0, 10, 1), rt(kG, 3, 0, 4),
    rt(kB, 1, 0, 4),  rt(kB, 0, 10, 1), rt(kB, 3, 1, 1),  rt(kB, 2, 0, 4), rt(kR, 2, 0, 4),
    rt(kB, 3, 0, 1),  rt(kB, 3, 2, 1),  rt(kR, 3, 0, 4),  rt(kG, 2, 4, 1), rt(kB, 3, 3, 1),
    kD,
};
constexpr Run kLayout5[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 4), rt(kR, 0, 10, 1),
    rt(kB, 2, 4, 1),  rt(kG, 2, 0, 4),  rt(kG, 1, 0, 4),  rt(kG, 0, 10, 1), rt(kB, 3, 0, 1),
    rt(kG, 3, 0, 4),  rt(kB, 1, 0, 5),  rt(kB, 0, 10, 1), rt(kB, 2, 0, 4), rt(kR, 2, 0, 4),
    rt(kB, 3, 1, 1),  rt(kB, 3, 2, 1),  rt(kR, 3, 0, 4),  rt(kB, 3, 4, 1), rt(kB, 3, 3, 1),
    kD,
};
constexpr Run kLayout6[] = {
    rt(kR, 0, 0, 9),  rt(kB, 2, 4, 1), rt(kG, 0, 0, 9), rt(kG, 2, 4, 1), rt(kB, 0, 0, 9),
    rt(kB, 3, 4, 1),  rt(kR, 1, 0, 5), rt(kG, 3, 4, 1), rt(kG, 2, 0, 4), rt(kG, 1, 0, 5),
    rt(kB, 3, 0, 1),  rt(kG, 3, 0, 4), rt(kB, 1, 0, 5), rt(kB, 3, 1, 1), rt(kB, 2, 0, 4),
    rt(kR, 2, 0, 5),  rt(kB, 3, 2, 1), rt(kR, 3, 0, 5), rt(kB, 3, 3, 1), kD,
};
constexpr Run kLayout7[] = {
    rt(kR, 0, 0, 8),  rt(kG, 3, 4, 1), rt(kB, 2, 4, 1), rt(kG, 0, 0, 8), rt(kB, 3, 2, 1),
    rt(kG, 2, 4, 1),  rt(kB, 0, 0, 8), rt(kB, 3, 3, 1), rt(kB, 3, 4, 1), rt(kR, 1, 0, 6),
    rt(kG, 2, 0, 4),  rt(kG, 1, 0, 5), rt(kB, 3, 0, 1), rt(kG, 3, 0, 4), rt(kB, 1, 0, 5),
    rt(kB, 3, 1, 1),  rt(kB, 2, 0, 4), rt(kR, 2, 0, 6), rt(kR, 3, 0, 6), kD,
};
constexpr Run kLayout8[] = {
    rt(kR, 0, 0, 8),  rt(kB, 3, 0, 1), rt(kB, 2, 4, 1), rt(kG, 0, 0, 8), rt(kG, 2, 5, 1),
    rt(kG, 2, 4, 1),  rt(kB, 0, 0, 8), rt(kG, 3, 5, 1), rt(kB, 3, 4, 1), rt(kR, 1, 0, 5),
    rt(kG, 3, 4, 1),  rt(kG, 2, 0, 4), rt(kG, 1, 0, 6), rt(kG, 3, 0, 4), rt(kB, 1, 0, 5),
    rt(kB, 3, 1, 1),  rt(kB, 2, 0, 4), rt(kR, 2, 0, 5), rt(kB, 3, 2, 1), rt(kR, 3, 0, 5),
    rt(kB, 3, 3, 1),  kD,
};
constexpr Run kLayout9[] = {
    rt(kR, 0, 0, 8),  rt(kB, 3, 1, 1), rt(kB, 2, 4, 1), rt(kG, 0, 0, 8), rt(kB, 2, 5, 1),
    rt(kG, 2, 4, 1),  rt(kB, 0, 0, 8), rt(kB, 3, 5, 1), rt(kB, 3, 4, 1), rt(kR, 1, 0, 5),
    rt(kG, 3, 4, 1),  rt(kG, 2, 0, 4), rt(kG, 1, 0, 5), rt(kB, 3, 0, 1), rt(kG, 3, 0, 4),
    rt(kB, 1, 0, 6),  rt(kB, 2, 0, 4), rt(kR, 2, 0, 5), rt(kB, 3, 2, 1), rt(kR, 3, 0, 5),
    rt(kB, 3, 3, 1),  kD,
};
constexpr Run kLayout10[] = {
    rt(kR, 0, 0, 6),  rt(kG, 3, 4, 1), rt(kB, 3, 0, 1), rt(kB, 3, 1, 1), rt(kB, 2, 4, 1),
    rt(kG, 0, 0, 6),  rt(kG, 2, 5, 1), rt(kB, 2, 5, 1), rt(kB, 3, 2, 1), rt(kG, 2, 4, 1),
    rt(kB, 0, 0, 6),  rt(kG, 3, 5, 1), rt(kB, 3, 3, 1), rt(kB, 3, 5, 1), rt(kB, 3, 4, 1),
    rt(kR, 1, 0, 6),  rt(kG, 2, 0, 4), rt(kG, 1, 0, 6), rt(kG, 3, 0, 4), rt(kB, 1, 0, 6),
    rt(kB, 2, 0, 4),  rt(kR, 2, 0, 6), rt(kR, 3, 0, 6), kD,
};
constexpr Run kLayout11[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10),
    rt(kR, 1, 0, 10), rt(kG, 1, 0, 10), rt(kB, 1, 0, 10),
};
constexpr Run kLayout12[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 9), rt(kR, 0, 10, 1),
    rt(kG, 1, 0, 9),  rt(kG, 0, 10, 1), rt(kB, 1, 0, 9),  rt(kB, 0, 10, 1),
};
constexpr Run kLayout13[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 8), rt(kR, 0, 10, 2),
    rt(kG, 1, 0, 8),  rt(kG, 0, 10, 2), rt(kB, 1, 0, 8),  rt(kB, 0, 10, 2),
};
constexpr Run kLayout14[] = {
    rt(kR, 0, 0, 10), rt(kG, 0, 0, 10), rt(kB, 0, 0, 10), rt(kR, 1, 0, 4), rt(kR, 0, 10, 6),
    rt(kG, 1, 0, 4),  rt(kG, 0, 10, 6), rt(kB, 1, 0, 4),  rt(kB, 0, 10, 6),
};

constexpr std::array<std::span<const Run>, 15> kBc6Layout{
    std::span<const Run>{}, kLayout1,  kLayout2,  kLayout3,  kLayout4,
    kLayout5,  kLayout6,  kLayout7,  kLayout8,  kLayout9,
    kLayout10, kLayout11, kLayout12, kLayout13, kLayout14,
};

constexpr std::array<std::uint8_t, 12> kWidths1{7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
constexpr std::array<std::uint8_t, 12> kWidths3{3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
constexpr std::array<std::uint8_t, 12> kWidths4{7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::array<std::uint8_t, 12> kWidths5{3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::array<std::uint8_t, 16> kWidths6{3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
constexpr std::array<std::uint8_t, 16> kWidths7{3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};

struct Widths {
    std::array<std::uint8_t, 8> ep;
    std::array<std::uint8_t, 32> ix;
    bool paired;
};

constexpr Widths make_widths(std::array<std::uint8_t, 8> ep, std::size_t three_bit,
                             std::size_t two_bit, bool paired) {
    Widths w{ep, {}, paired};
    for (std::size_t k = 0; k < three_bit + two_bit; ++k) w.ix[k] = k < three_bit ? 3 : 2;
    return w;
}

constexpr Widths kBc1Widths = make_widths({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
constexpr Widths kBc3Widths = make_widths({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
constexpr Widths kBc4Widths = make_widths({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
constexpr Widths kBc5Widths = make_widths({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);

struct Reader {
    const std::uint8_t* at;
    std::size_t left;

    bool bytes(std::uint8_t* out, std::size_t count) {
        if (left < count) return false;
        std::memcpy(out, at, count);
        at += count;
        left -= count;
        return true;
    }
    bool u8(std::uint8_t& value) { return bytes(&value, 1); }
    bool u16(std::uint16_t& value) {
        std::uint8_t raw[2]{};
        if (!bytes(raw, 2)) return false;
        value = static_cast<std::uint16_t>(raw[0] | raw[1] << 8);
        return true;
    }
    bool i16(std::int16_t& value) {
        std::uint16_t raw = 0;
        if (!u16(raw)) return false;
        value = static_cast<std::int16_t>(raw);
        return true;
    }
};

std::uint8_t interleave(std::uint8_t a, std::uint8_t b) {
    const auto spread = [](std::uint8_t v) {
        std::uint8_t r = 0;
        for (unsigned k = 0; k < 4; ++k) r = static_cast<std::uint8_t>(r | ((v >> k & 1u) << (2 * k)));
        return r;
    };
    return static_cast<std::uint8_t>(spread(a) | spread(b) << 1);
}

std::pair<std::size_t, std::size_t> references(const std::uint8_t* s, bool paired) {
    std::uint8_t d = 0;
    std::uint8_t ix = 0;
    if (paired) {
        d = interleave(s[0], s[1]);
        ix = interleave(s[2], s[3]);
    } else {
        d = s[0];
        ix = s[1];
    }
    const std::uint8_t delta =
        static_cast<std::uint8_t>((d >> 1) ^ static_cast<std::uint8_t>(0u - (d & 1u)));
    return {static_cast<std::size_t>(static_cast<std::uint8_t>(ix + delta)),
            static_cast<std::size_t>(ix)};
}

std::uint8_t zigzag(std::uint8_t v, unsigned bits) {
    const std::uint8_t mask = static_cast<std::uint8_t>((1u << bits) - 1);
    const std::uint8_t sign = (v & 1u) != 0 ? mask : 0;
    return static_cast<std::uint8_t>(((v >> 1) ^ sign) & mask);
}

void quads(const std::array<std::uint8_t, 256>& rank, const std::uint8_t* s, std::uint8_t* ix) {
    for (std::size_t q = 0; q < 4; ++q) {
        const std::uint8_t v = rank[s[q]];
        const std::size_t base = (q >> 1) * 8 + (q & 1) * 2;
        const std::size_t positions[4] = {0, 1, 4, 5};
        for (std::size_t k = 0; k < 4; ++k) ix[base + positions[k]] = static_cast<std::uint8_t>(v >> (2 * k) & 3u);
    }
}

void pairs(const std::uint8_t* rank, const std::uint8_t* s, std::size_t count, std::uint8_t* ix) {
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t v = rank[s[k] & 63u];
        ix[2 * k] = static_cast<std::uint8_t>(v & 7u);
        ix[2 * k + 1] = static_cast<std::uint8_t>(v >> 4 & 7u);
    }
}

Plain bc1_residual(const Tables& t, const std::uint8_t* s) {
    Plain v;
    const unsigned bits[6] = {5, 6, 5, 5, 6, 5};
    for (std::size_t k = 0; k < 6; ++k) v.ep[k] = zigzag(s[2 + k], bits[k]);
    quads(t.rank1, s + 8, v.ix.data());
    return v;
}

Plain bc3_residual(const Tables& t, const std::uint8_t* s) {
    Plain v;
    v.ep = {zigzag(s[14], 5), zigzag(s[16], 6), zigzag(s[18], 5), zigzag(s[15], 5),
            zigzag(s[17], 6), zigzag(s[19], 5), zigzag(s[4], 8),  zigzag(s[5], 8)};
    pairs(t.rank3a.data(), s + 6, 8, v.ix.data());
    quads(t.rank3c, s + 20, v.ix.data() + 16);
    return v;
}

Plain bc4_residual(const Tables& t, const std::uint8_t* s) {
    Plain v;
    v.ep[0] = zigzag(s[2], 8);
    v.ep[1] = zigzag(s[3], 8);
    pairs(t.rank4.data(), s + 4, 8, v.ix.data());
    return v;
}

Plain bc5_residual(const Tables& t, const std::uint8_t* s) {
    Plain v;
    for (std::size_t k = 0; k < 4; ++k) v.ep[k] = zigzag(s[4 + k], 8);
    pairs(t.rank5.data(), s + 8, 16, v.ix.data());
    return v;
}

template <typename T>
const T& pick(std::size_t i, std::size_t r, const std::array<T, kBcpackHistory>& history,
              const std::array<T, kBcpackDictionary>& dictionary, const T& zero) {
    if (r == 0) return zero;
    if (r <= i) return history[(i - r) % kBcpackHistory];
    const std::size_t at = r - i - 1;
    return at < dictionary.size() ? dictionary[at] : zero;
}

void bc1_color(const std::array<std::uint8_t, 8>& ep, const std::uint8_t* ix,
               std::span<std::uint8_t> out) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(ep[0] << 11 | ep[1] << 5 | ep[2]);
    const std::uint16_t c1 = static_cast<std::uint16_t>(ep[3] << 11 | ep[4] << 5 | ep[5]);
    std::uint32_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p) bits |= static_cast<std::uint32_t>(kBc1Code[ix[p] & 3]) << (2 * p);
    out[0] = static_cast<std::uint8_t>(c0 & 0xff);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1 & 0xff);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    for (std::size_t k = 0; k < 4; ++k) out[4 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

void bc4_alpha(std::uint8_t e0, std::uint8_t e1, const std::uint8_t* ix, std::span<std::uint8_t> out) {
    std::uint64_t bits = 0;
    for (std::size_t p = 0; p < 16; ++p) bits |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
    const std::uint64_t v = static_cast<std::uint64_t>(e0 ^ 0x80) |
                            static_cast<std::uint64_t>(e1 ^ 0x80) << 8 | bits;
    for (std::size_t k = 0; k < 8; ++k) out[k] = static_cast<std::uint8_t>(v >> (8 * k));
}

void bc1_pack(const Plain& v, std::span<std::uint8_t> out) { bc1_color(v.ep, v.ix.data(), out); }

void bc3_pack(const Plain& v, std::span<std::uint8_t> out) {
    bc4_alpha(v.ep[6], v.ep[7], v.ix.data(), out.first(8));
    bc1_color(v.ep, v.ix.data() + 16, out.subspan(8));
}

void bc4_pack(const Plain& v, std::span<std::uint8_t> out) {
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out);
}

void bc5_pack(const Plain& v, std::span<std::uint8_t> out) {
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out.first(8));
    bc4_alpha(v.ep[2], v.ep[3], v.ix.data() + 16, out.subspan(8));
}

std::array<std::int16_t, 2> nibbles(std::uint8_t b, bool odd_first) {
    const auto half = [](std::uint8_t v) {
        std::uint8_t r = 0;
        for (unsigned k = 0; k < 4; ++k) r = static_cast<std::uint8_t>(r | ((v >> (2 * k) & 1u) << k));
        return r;
    };
    const auto z = [](std::uint8_t u) {
        return static_cast<std::int16_t>(static_cast<std::int16_t>(u >> 1) ^
                                         -static_cast<std::int16_t>(u & 1u));
    };
    const std::int16_t even = z(half(b));
    const std::int16_t odd = z(half(static_cast<std::uint8_t>(b >> 1)));
    if (odd_first) return {odd, even};
    return {even, odd};
}

std::int16_t add_index(std::int16_t cur, std::uint8_t bits, std::int16_t ref, std::uint8_t ref_bits) {
    const auto up = [](std::int16_t v, std::uint8_t b) {
        if (b == 0 || b > 16) return std::uint16_t{0};
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(v) << (16 - b));
    };
    const std::uint16_t sum = static_cast<std::uint16_t>(up(cur, bits) + up(ref, ref_bits));
    if (bits == 0) return 0;
    return static_cast<std::int16_t>(static_cast<std::int16_t>(sum) >> (16 - bits));
}

struct Bits {
    std::array<std::uint64_t, 2> v{};
    std::uint32_t at = 0;

    void put(std::uint64_t value, std::uint32_t n) {
        if (n > 0 && at < 128) {
            const std::uint64_t mask = n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
            value &= mask;
            const unsigned shift = at & 63u;
            const unsigned word = at >> 6;
            v[word] |= value << shift;
            if (shift != 0 && word == 0) v[1] |= value >> (64 - shift);
        }
        at += n;
    }

    void bytes(std::span<std::uint8_t> out) const {
        for (std::size_t k = 0; k < 8; ++k) {
            out[k] = static_cast<std::uint8_t>(v[0] >> (8 * k));
            out[8 + k] = static_cast<std::uint8_t>(v[1] >> (8 * k));
        }
    }
};

bool pbits(std::size_t mode) { return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7; }

std::array<std::uint8_t, 4> bc7_precision(std::uint16_t ctrl) {
    const std::size_t mode = ctrl & 7;
    const std::size_t rot = (ctrl >> 3) & 3;
    std::array<std::uint8_t, 4> p = kBc7Bits[mode];
    if (rot != 0) {
        std::swap(p[3], p[rot - 1]);
    } else if (pbits(mode)) {
        p[0] = static_cast<std::uint8_t>(p[0] + 1);
    }
    return p;
}

std::uint8_t bc6_index_bits(std::uint8_t mode) {
    if (mode >= 1 && mode <= 10) return 3;
    if (mode >= 11 && mode <= 14) return 4;
    return 0;
}

void bc7_pack(const Bc7Ep& ep, const Bc7Ix& ix, const std::array<std::uint8_t, 4>& prec,
              std::span<std::uint8_t> out) {
    const std::uint16_t ctrl = ep.ctrl;
    const std::size_t mode = ctrl & 7;
    const std::size_t subsets = (ctrl & 0xff) >> 6;
    const std::size_t part = (ctrl >> 8) & 63;
    const std::size_t rot = (ctrl >> 3) & 3;
    Bits w;
    w.put(std::uint64_t{1} << mode, static_cast<std::uint32_t>(mode + 1));
    switch (mode) {
        case 0:
            w.put(part, 4);
            break;
        case 1:
        case 2:
        case 3:
        case 7:
            w.put(part, 6);
            break;
        case 4:
            w.put(rot, 2);
            w.put((ctrl >> 5) & 1u, 1);
            break;
        case 5:
            w.put(rot, 2);
            break;
        default:
            break;
    }
    const std::size_t ends = 2 * subsets;
    const auto field = [&](std::size_t row, std::size_t col, std::uint8_t p) {
        return static_cast<std::uint64_t>(ep.rows[row][col] >> (16 - p));
    };
    std::array<std::size_t, 4> order{0, 1, 2, 3};
    if (rot != 0) std::swap(order[3], order[rot - 1]);
    for (const std::size_t ch : order) {
        const std::uint8_t p = prec[ch];
        if (p == 0) continue;
        const std::uint32_t drop = ch == 0 && pbits(mode) ? 1u : 0u;
        for (std::size_t e = 0; e < ends; ++e)
            w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, static_cast<std::uint32_t>(p) - drop);
    }
    if (mode == 1) {
        for (std::size_t row = 0; row < subsets; ++row) w.put(field(row, 0, prec[0]) & 1u, 1);
    } else if (pbits(mode)) {
        for (std::size_t e = 0; e < ends; ++e) w.put(field(e / 2, e % 2, prec[0]) & 1u, 1);
    }
    std::array<std::uint8_t, 3> anchors{};
    std::size_t anchor_count = 1;
    if (subsets == 2) {
        anchors[1] = kAnchor2[part];
        anchor_count = 2;
    } else if (subsets == 3) {
        anchors[1] = kAnchor3A[part];
        anchors[2] = kAnchor3B[part];
        anchor_count = 3;
    }
    const auto indices = [&](std::size_t set) {
        const std::uint32_t nbits = ix.bits[set];
        for (std::size_t p = 0; p < 16; ++p) {
            bool anchor = false;
            for (std::size_t a = 0; a < anchor_count; ++a) anchor = anchor || anchors[a] == p;
            const std::uint32_t n = nbits - (anchor ? 1u : 0u);
            w.put(static_cast<std::uint16_t>(ix.ix[set][p]), n);
        }
    };
    switch (mode) {
        case 4:
            indices(1);
            indices(0);
            break;
        case 5:
            indices(0);
            indices(1);
            break;
        default:
            indices(0);
            break;
    }
    w.bytes(out);
}

template <typename Residual, typename Pack>
std::optional<BcpackError> decode_plain(const Tables& t, std::span<const std::uint8_t> records,
                                        std::size_t stride, std::span<std::uint8_t> dst,
                                        std::size_t block,
                                        const std::array<Plain, kBcpackDictionary>& dictionary,
                                        const Widths& w, Residual residual, Pack pack) {
    std::array<Plain, kBcpackHistory> history{};
    const Plain zero{};
    const std::size_t blocks = dst.size() / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, w.paired);
        const Plain& er = pick(i, ep_ref, history, dictionary, zero);
        const Plain& xr = pick(i, ix_ref, history, dictionary, zero);
        Plain v = residual(t, s);
        for (std::size_t k = 0; k < 8; ++k)
            v.ep[k] = static_cast<std::uint8_t>((v.ep[k] + er.ep[k]) & ((1u << w.ep[k]) - 1u));
        for (std::size_t k = 0; k < 32; ++k)
            v.ix[k] = static_cast<std::uint8_t>((v.ix[k] + xr.ix[k]) & ((1u << w.ix[k]) - 1u));
        history[i % kBcpackHistory] = v;
        pack(v, dst.subspan(i * block, block));
    }
    return std::nullopt;
}

std::optional<BcpackError> decode_bc6(std::span<const std::uint8_t> records,
                                      std::size_t stride, std::span<std::uint8_t> dst,
                                      std::size_t block,
                                      const std::array<Bc6Block, kBcpackDictionary>& dictionary) {
    std::array<Bc6Block, kBcpackHistory> history{};
    Bc6Block zero;
    zero.ix.mode = 11;
    const auto z = [](std::uint16_t v) {
        return static_cast<std::uint16_t>((v >> 1) ^ ((v & 1u) != 0 ? 0xffffu : 0u));
    };
    const auto wide = [](std::uint8_t a, std::uint8_t b) {
        std::uint16_t acc = 0;
        for (unsigned k = 0; k < 8; ++k)
            acc = static_cast<std::uint16_t>(acc | static_cast<std::uint16_t>((a >> k & 1u) << (2 * k)) |
                                             static_cast<std::uint16_t>((b >> k & 1u) << (2 * k + 1)));
        return acc;
    };
    const std::size_t blocks = dst.size() / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const std::uint8_t mode = static_cast<std::uint8_t>(s[30] + 1);
        if (mode < 1 || mode > 14) return BcpackError{"BC6H mode out of range"};
        const bool two = mode < 11;
        std::array<unsigned, 4> shift{};
        for (std::size_t c = 0; c < 4; ++c) shift[c] = 16u - kBc6Bits[mode][c];
        const std::array<std::uint16_t, 3> e0{z(wide(s[4], s[5])), z(wide(s[6], s[7])),
                                              z(wide(s[8], s[9]))};
        const std::array<std::uint16_t, 3> e1{z(wide(s[10], s[11])), z(wide(s[12], s[13])),
                                              z(wide(s[14], s[15]))};
        std::array<std::uint16_t, 3> e2{};
        std::array<std::uint16_t, 3> e3{};
        if (two) {
            e2 = {z(s[16]), z(s[18]), z(s[20])};
            e3 = {z(s[17]), z(s[19]), z(s[21])};
        } else {
            e2 = e0;
            e3 = e1;
        }
        const Bc6Ep& r = pick(i, ep_ref, history, dictionary, zero).ep;
        Bc6Ep ep;
        const auto mask = [](unsigned sh) { return static_cast<std::uint16_t>(0xffffu << sh); };
        for (std::size_t c = 0; c < 3; ++c)
            ep.e[0][c] = static_cast<std::uint16_t>(
                (r.e[0][c] + static_cast<std::uint16_t>(e0[c] << shift[0])) & mask(shift[0]));
        const std::array<std::array<std::uint16_t, 3>, 3> others{e1, e2, e3};
        const std::size_t count = two ? 3 : 1;
        for (std::size_t k = 0; k < count; ++k)
            for (std::size_t c = 0; c < 3; ++c)
                ep.e[k + 1][c] = static_cast<std::uint16_t>(
                    (r.e[k + 1][c] + static_cast<std::uint16_t>(others[k][c] << shift[1 + c])) &
                    mask(shift[1 + c]));
        if (!two) {
            for (std::size_t c = 0; c < 3; ++c)
                ep.e[2][c] = static_cast<std::uint16_t>((ep.e[0][c] >> shift[0]) << shift[1]);
            ep.e[3] = ep.e[1];
        }
        const Bc6Ix& rx = pick(i, ix_ref, history, dictionary, zero).ix;
        const std::uint8_t bits = bc6_index_bits(mode);
        Bc6Ix ix;
        ix.mode = mode;
        for (std::size_t k = 0; k < 8; ++k) {
            const auto pair = nibbles(s[22 + k], k < 6);
            for (std::size_t h = 0; h < 2; ++h) {
                const std::size_t p = 2 * k + h;
                ix.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
            }
        }
        history[i % kBcpackHistory] = Bc6Block{ep, ix};
        const auto field = [&](std::size_t k, std::size_t c) {
            const unsigned sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<std::uint64_t>(
                static_cast<std::uint16_t>(ep.e[k][c] ^ 0x8000) >> sh);
        };
        Bits w;
        w.put(kBc6Code[mode].first, kBc6Code[mode].second);
        const std::uint8_t part = static_cast<std::uint8_t>(s[31] & 31u);
        for (const Run& run : kBc6Layout[mode]) {
            if (run.ch == 3)
                w.put(part, 5);
            else
                w.put(field(run.ep, run.ch) >> run.first, run.len);
        }
        const std::uint8_t anchor = two ? kAnchor2[part] : 0;
        for (std::size_t p = 0; p < 16; ++p) {
            const bool short_index = p == 0 || p == anchor;
            const std::uint16_t v = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(ix.ix[p]) + (1u << (bits - 1)));
            w.put(v, static_cast<std::uint32_t>(bits) - (short_index ? 1u : 0u));
        }
        w.bytes(dst.subspan(i * block, block));
    }
    return std::nullopt;
}

std::optional<BcpackError> decode_bc7(const Tables& t, std::span<const std::uint8_t> records,
                                      std::size_t stride, std::span<std::uint8_t> dst,
                                      std::size_t block,
                                      const std::array<Bc7Block, kBcpackDictionary>& dictionary) {
    std::array<Bc7Block, kBcpackHistory> history{};
    const Bc7Block zero{};
    const std::size_t blocks = dst.size() / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const std::uint16_t ctrl =
            t.ctrl7[static_cast<std::size_t>(s[31] & 15u) << 5 | static_cast<std::size_t>(s[30] & 31u)];
        if (ctrl == 0) return BcpackError{"unused BC7 control code"};
        const std::size_t mode = ctrl & 7;
        const std::size_t subsets = (ctrl & 0xff) >> 6;
        const std::array<std::uint8_t, 4> prec = bc7_precision(ctrl);
        const bool three = subsets == 3;
        const auto or0 = [three](std::uint8_t v) { return three ? std::uint8_t{0} : v; };
        const auto if3 = [three](std::uint8_t v) { return three ? v : std::uint8_t{0}; };
        const std::array<std::array<std::uint8_t, 8>, 3> stored{{
            {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
            {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
            {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0},
        }};
        const Bc7Ep& r = pick(i, ep_ref, history, dictionary, zero).ep;
        Bc7Ep ep;
        ep.ctrl = ctrl;
        const std::size_t common = std::min(subsets, static_cast<std::size_t>((r.ctrl & 0xff) >> 6));
        const std::size_t columns = prec[3] > 0 ? 8 : 6;
        for (std::size_t c = 0; c < columns; ++c) {
            const std::uint8_t p = prec[c / 2];
            if (p == 0) continue;
            const std::uint16_t mask = static_cast<std::uint16_t>(0xffffu << (16 - p));
            for (std::size_t row = 0; row < subsets; ++row) {
                const std::uint8_t v = stored[row][c];
                const std::uint16_t residual = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(v >> 1) ^
                                               ((v & 1u) != 0 ? 0xffffu : 0u))
                    << (16 - p));
                const std::uint16_t base = r.rows[row < common ? row : 0][c] & mask;
                ep.rows[row][c] = static_cast<std::uint16_t>(base + residual);
            }
        }
        const std::array<std::uint8_t, 2> bits = kBc7Index[mode];
        const Bc7Ix& rx = pick(i, ix_ref, history, dictionary, zero).ix;
        Bc7Ix ix;
        ix.bits = bits;
        constexpr std::size_t kOffsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
        for (std::size_t set = 0; set < 2; ++set) {
            if (bits[set] == 0) continue;
            for (std::size_t k = 0; k < 4; ++k) {
                const std::size_t o = kOffsets[set][k];
                for (std::size_t b = 0; b < 2; ++b) {
                    const auto pair = nibbles(s[o + b], k < 3);
                    for (std::size_t h = 0; h < 2; ++h) {
                        const std::size_t p = 4 * k + 2 * b + h;
                        ix.ix[set][p] = add_index(pair[h], bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
            }
        }
        history[i % kBcpackHistory] = Bc7Block{ep, ix};
        bc7_pack(ep, ix, prec, dst.subspan(i * block, block));
    }
    return std::nullopt;
}

}  // namespace

std::size_t bucket_of(const Tables& tables, std::uint8_t model, std::uint32_t slot) {
    const auto& start = tables.start[model];
    std::size_t bucket = tables.bucket[model][slot >> 8];
    while (start[bucket + 1] <= slot) ++bucket;
    return bucket;
}

std::optional<BcpackError> BcpackLane::step(const Tables& t) {
    if (phase != 3) {
        if (!latch) return std::nullopt;
        const std::uint8_t b = *latch;
        latch.reset();
        need = true;
        switch (phase) {
            case 0: {
                const std::uint8_t group = static_cast<std::uint8_t>(width - (b >> 6));
                model = static_cast<std::uint8_t>(group * 16 + (b & 15u));
                reset = (b & 0x10u) != 0;
                long_fragment = (b & 0x20u) != 0;
                left = 0;
                phase = 1;
                break;
            }
            case 1:
                if (long_fragment) {
                    left = b;
                    phase = 2;
                } else {
                    left = static_cast<std::uint32_t>(b) + 1;
                    phase = 3;
                }
                break;
            default:
                left = ((static_cast<std::uint32_t>(b) << 8) | left) + 257;
                phase = 3;
                break;
        }
        return std::nullopt;
    }
    std::uint32_t state = this->x;
    if (state < kBcpackLow && latch) {
        left -= 1;
        state = state << 8 | *latch;
        latch.reset();
    }
    need = left != 0 && !latch;
    if (state > kBcpackLow) {
        const auto& freq = t.freq[model];
        const auto& start = t.start[model];
        const std::uint32_t slot = state & 0xffffu;
        const std::size_t bucket = bucket_of(t, model, slot);
        std::uint8_t sym = 0;
        std::uint32_t f = 0;
        std::uint32_t rem = 0;
        if (bucket == 0) {
            sym = 0;
            f = freq[0] - 1;
            rem = slot - 1;
        } else {
            const std::uint32_t offset = slot - start[bucket];
            const std::uint32_t q = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(offset) * t.recip[model][bucket] >> 32);
            sym = static_cast<std::uint8_t>((std::uint32_t{1} << (bucket - 1)) + q);
            f = freq[bucket];
            rem = offset - q * freq[bucket];
        }
        this->x = (state >> 16) * f + rem;
        this->out = sym;
    } else if (left != 0) {
        this->x = state;
    } else {
        phase = 0;
        need = !latch;
        if (reset) this->x = 0;
    }
    return std::nullopt;
}

std::optional<Tables> Tables::parse(std::span<const std::uint8_t> bytes, BcpackError* error) {
    const auto fail = [error](const char* why) -> std::optional<Tables> {
        if (error) *error = BcpackError{why};
        return std::nullopt;
    };
    if (bytes.size() != kBcpackTablesSize) return fail("table file has the wrong size");
    Reader r{bytes.data(), bytes.size()};
    Tables t;
    for (std::size_t m = 0; m < kBcpackModels; ++m) {
        for (std::size_t b = 0; b < kBcpackBuckets; ++b) {
            std::uint16_t f = 0;
            if (!r.u16(f)) return fail("table file is truncated");
            t.freq[m][b] = f;
        }
        const std::size_t used = m / 16 + 2;
        std::uint32_t at = 0;
        for (std::size_t b = 0; b < used; ++b) {
            const std::uint32_t f = t.freq[m][b];
            if (f == 0) return fail("a model has an empty bucket");
            t.start[m][b] = at;
            t.recip[m][b] = ((std::uint64_t{1} << 32) + f - 1) / f;
            at += f << (b > 0 ? b - 1 : 0);
        }
        if (at != 1u << 16) return fail("a model does not tile 65536");
        for (std::size_t b = used; b <= kBcpackBuckets; ++b) t.start[m][b] = 1u << 16;
        for (std::size_t window = 0; window < 256; ++window) {
            const std::uint32_t first = static_cast<std::uint32_t>(window) << 8;
            std::uint8_t count = 0;
            for (std::size_t b = 1; b <= kBcpackBuckets; ++b)
                if (t.start[m][b] <= first) ++count;
            t.bucket[m][window] = count;
        }
    }
    if (!r.bytes(t.rank1.data(), t.rank1.size())) return fail("table file is truncated");
    if (!r.bytes(t.rank3a.data(), t.rank3a.size())) return fail("table file is truncated");
    if (!r.bytes(t.rank3c.data(), t.rank3c.size())) return fail("table file is truncated");
    if (!r.bytes(t.rank4.data(), t.rank4.size())) return fail("table file is truncated");
    if (!r.bytes(t.rank5.data(), t.rank5.size())) return fail("table file is truncated");
    for (auto& c : t.ctrl7)
        if (!r.u16(c)) return fail("table file is truncated");
    const auto plain = [&](std::size_t ep_bytes, std::size_t ix_bytes,
                           std::array<Plain, kBcpackDictionary>& dictionary) {
        for (auto& p : dictionary) {
            if (!r.bytes(p.ep.data(), ep_bytes)) return false;
            if (!r.bytes(p.ix.data(), ix_bytes)) return false;
        }
        return true;
    };
    if (!plain(6, 16, t.bc1)) return fail("table file is truncated");
    if (!plain(8, 32, t.bc3)) return fail("table file is truncated");
    if (!plain(2, 16, t.bc4)) return fail("table file is truncated");
    if (!plain(4, 32, t.bc5)) return fail("table file is truncated");
    for (auto& block : t.bc6) {
        std::uint8_t ignored = 0;
        if (!r.u8(ignored)) return fail("table file is truncated");
        for (auto& row : block.ep.e)
            for (auto& v : row)
                if (!r.u16(v)) return fail("table file is truncated");
        if (!r.u8(block.ix.mode)) return fail("table file is truncated");
        for (auto& v : block.ix.ix)
            if (!r.i16(v)) return fail("table file is truncated");
    }
    for (auto& block : t.bc7) {
        if (!r.u16(block.ep.ctrl)) return fail("table file is truncated");
        for (auto& row : block.ep.rows)
            for (auto& v : row)
                if (!r.u16(v)) return fail("table file is truncated");
        if (!r.u8(block.ix.bits[0])) return fail("table file is truncated");
        if (!r.u8(block.ix.bits[1])) return fail("table file is truncated");
        for (auto& set : block.ix.ix)
            for (auto& v : set)
                if (!r.i16(v)) return fail("table file is truncated");
    }
    return t;
}

std::optional<BcpackError> lanes(const Tables& tables, std::span<const std::uint8_t> widths,
                                 std::size_t per, std::size_t blocks, const BcpackFetch& fetch,
                                 std::span<std::uint8_t> records) {
    const std::size_t count = widths.size();
    if (count == 0 || count > 16) return BcpackError{"too many lanes"};
    const std::size_t stride = count * per;
    if (stride == 0) return BcpackError{"no lanes"};
    if (blocks > records.size() / stride) return BcpackError{"record buffer is too small"};
    std::array<BcpackLane, 16> lanes{};
    for (std::size_t j = 0; j < count; ++j) {
        lanes[j].width = widths[j];
        lanes[j].need = true;
    }
    const std::uint32_t full = (std::uint32_t{1} << count) - 1;
    std::size_t pos = 0;
    std::uint32_t done = 0;
    unsigned idle = 0;
    std::size_t at = 0;
    while (pos < blocks * per) {
        std::size_t taken = 0;
        bool moved = false;
        for (std::size_t j = 0; j < count; ++j) {
            BcpackLane& lane = lanes[j];
            const std::uint32_t bit = 1u << j;
            const std::size_t before = taken;
            if (!lane.stalled) {
                if (const auto error = lane.step(tables)) return error;
            }
            const bool waiting = lane.out.has_value() && (done & bit) != 0;
            if (lane.need && !waiting && taken < kBcpackRoundBytes) {
                if (const auto b = fetch(j)) {
                    lane.latch = *b;
                    ++taken;
                }
            }
            lane.stalled = (lane.need && before >= kBcpackRoundBytes) || waiting;
            if (!lane.stalled && lane.out) {
                const std::uint8_t sym = *lane.out;
                lane.out.reset();
                records[at + j * per] = sym;
                done |= bit;
                moved = true;
            }
        }
        if (done == full) {
            ++pos;
            at += pos % per == 0 ? stride - per + 1 : 1;
            done = 0;
            moved = true;
        }
        idle = (taken > 0 || moved) ? 0 : idle + 1;
        if (idle > 3) return BcpackError{"the stream ends early or is corrupt"};
    }
    return std::nullopt;
}

std::optional<BcpackError> decode(std::uint8_t bc, std::span<const std::uint8_t> src,
                                  std::span<std::uint8_t> dst, const Tables& tables) {
    std::size_t block = 0;
    switch (bc) {
        case 1:
        case 4:
            block = 8;
            break;
        case 3:
        case 5:
        case 6:
        case 7:
            block = 16;
            break;
        default:
            return BcpackError{"unsupported BC format"};
    }
    if (dst.empty() || dst.size() % block != 0)
        return BcpackError{"destination is not a whole number of blocks"};
    const std::size_t blocks = dst.size() / block;
    std::span<const std::uint8_t> widths;
    std::size_t per = 1;
    switch (bc) {
        case 1:
            widths = kWidths1;
            break;
        case 3:
            widths = kWidths3;
            per = 2;
            break;
        case 4:
            widths = kWidths4;
            break;
        case 5:
            widths = kWidths5;
            per = 2;
            break;
        case 6:
            widths = kWidths6;
            per = 2;
            break;
        default:
            widths = kWidths7;
            per = 2;
            break;
    }
    const std::size_t stride = widths.size() * per;
    std::vector<std::uint8_t> records;
    try {
        records.assign(blocks * stride, 0);
    } catch (const std::bad_alloc&) {
        return BcpackError{"not enough memory for the destination"};
    }
    std::size_t next = 0;
    const BcpackFetch fetch = [&src, &next](std::size_t) -> std::optional<std::uint8_t> {
        if (next >= src.size()) return std::nullopt;
        return src[next++];
    };
    if (const auto error = lanes(tables, widths, per, blocks, fetch, records)) return error;
    switch (bc) {
        case 1:
            return decode_plain(tables, records, stride, dst, block, tables.bc1, kBc1Widths,
                                bc1_residual, bc1_pack);
        case 3:
            return decode_plain(tables, records, stride, dst, block, tables.bc3, kBc3Widths,
                                bc3_residual, bc3_pack);
        case 4:
            return decode_plain(tables, records, stride, dst, block, tables.bc4, kBc4Widths,
                                bc4_residual, bc4_pack);
        case 5:
            return decode_plain(tables, records, stride, dst, block, tables.bc5, kBc5Widths,
                                bc5_residual, bc5_pack);
        case 6:
            return decode_bc6(records, stride, dst, block, tables.bc6);
        default:
            return decode_bc7(tables, records, stride, dst, block, tables.bc7);
    }
}

}  // namespace g6shim::xcrdapi
