// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace g6shim::xcrdapi {

inline constexpr std::size_t kBcpackModels = 128;
inline constexpr std::size_t kBcpackBuckets = 9;
inline constexpr std::size_t kBcpackHistory = 256;
inline constexpr std::size_t kBcpackDictionary = 255;
inline constexpr std::uint32_t kBcpackLow = 1u << 24;
inline constexpr std::size_t kBcpackRoundBytes = 8;
inline constexpr std::size_t kBcpackTablesSize =
    kBcpackModels * kBcpackBuckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2 +
    kBcpackDictionary * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66);

struct BcpackError {
    const char* reason;
};

struct Plain {
    std::array<std::uint8_t, 8> ep{};
    std::array<std::uint8_t, 32> ix{};
};

struct Bc6Ep {
    std::array<std::array<std::uint16_t, 3>, 4> e{};
};

struct Bc6Ix {
    std::uint8_t mode = 0;
    std::array<std::int16_t, 16> ix{};
};

struct Bc7Ep {
    std::uint16_t ctrl = 0;
    std::array<std::array<std::uint16_t, 8>, 3> rows{};
};

struct Bc7Ix {
    std::array<std::uint8_t, 2> bits{};
    std::array<std::array<std::int16_t, 16>, 2> ix{};
};

struct Bc6Block {
    Bc6Ep ep;
    Bc6Ix ix;
};

struct Bc7Block {
    Bc7Ep ep;
    Bc7Ix ix;
};

class Tables {
public:
    static std::optional<Tables> parse(std::span<const std::uint8_t> bytes, BcpackError* error);

    std::array<std::array<std::uint32_t, kBcpackBuckets>, kBcpackModels> freq{};
    std::array<std::array<std::uint32_t, kBcpackBuckets>, kBcpackModels> start{};
    std::array<std::array<std::uint64_t, kBcpackBuckets>, kBcpackModels> recip{};
    std::array<std::uint8_t, 256> rank1{};
    std::array<std::uint8_t, 64> rank3a{};
    std::array<std::uint8_t, 256> rank3c{};
    std::array<std::uint8_t, 64> rank4{};
    std::array<std::uint8_t, 64> rank5{};
    std::array<std::uint16_t, 512> ctrl7{};
    std::array<Plain, kBcpackDictionary> bc1{};
    std::array<Plain, kBcpackDictionary> bc3{};
    std::array<Plain, kBcpackDictionary> bc4{};
    std::array<Plain, kBcpackDictionary> bc5{};
    std::array<Bc6Block, kBcpackDictionary> bc6{};
    std::array<Bc7Block, kBcpackDictionary> bc7{};
};

using BcpackFetch = std::function<std::optional<std::uint8_t>(std::size_t)>;

std::optional<BcpackError> decode(std::uint8_t bc, std::span<const std::uint8_t> src,
                                  std::span<std::uint8_t> dst, const Tables& tables);

std::optional<BcpackError> lanes(const Tables& tables, std::span<const std::uint8_t> widths,
                                 std::size_t per, std::size_t blocks, const BcpackFetch& fetch,
                                 std::span<std::uint8_t> records);

}  // namespace g6shim::xcrdapi
