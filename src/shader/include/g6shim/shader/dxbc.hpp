// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::shader {

struct FourCC {
    std::array<std::uint8_t, 4> bytes{};

    constexpr FourCC() = default;
    constexpr FourCC(char a, char b, char c, char d)
        : bytes{static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
                static_cast<std::uint8_t>(d)} {}

    std::string text() const;

    friend bool operator==(const FourCC&, const FourCC&) = default;
};

inline constexpr FourCC kDxil{'D', 'X', 'I', 'L'};
inline constexpr FourCC kAccc{'A', 'C', 'C', 'C'};
inline constexpr FourCC kStat{'S', 'T', 'A', 'T'};
inline constexpr FourCC kRts0{'R', 'T', 'S', '0'};
inline constexpr FourCC kPsv0{'P', 'S', 'V', '0'};
inline constexpr FourCC kIsg1{'I', 'S', 'G', '1'};
inline constexpr FourCC kOsg1{'O', 'S', 'G', '1'};
inline constexpr FourCC kPsg1{'P', 'S', 'G', '1'};
inline constexpr FourCC kSfi0{'S', 'F', 'I', '0'};
inline constexpr FourCC kHash{'H', 'A', 'S', 'H'};
inline constexpr FourCC kIldb{'I', 'L', 'D', 'B'};
inline constexpr FourCC kIldn{'I', 'L', 'D', 'N'};

struct Part {
    FourCC fourcc;
    std::size_t offset = 0;
    std::span<const std::uint8_t> data;
};

struct Container {
    std::array<std::uint8_t, 16> digest{};
    std::pair<std::uint16_t, std::uint16_t> version{};
    std::vector<Part> parts;

    static Container parse(std::span<const std::uint8_t> data);
    const Part* part(FourCC fourcc) const;
};

}  // namespace g6shim::shader
