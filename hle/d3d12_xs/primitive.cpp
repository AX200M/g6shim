// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>

namespace g6shim::d3d12_xs::primitive {

namespace {

enum class Kind {
    kF32,
    kF16,
    kUnorm16,
    kUnorm8,
};

std::size_t bytes(Kind kind) {
    switch (kind) {
    case Kind::kF32: return 4;
    case Kind::kF16: return 2;
    case Kind::kUnorm16: return 2;
    case Kind::kUnorm8: return 1;
    }
    return 0;
}

std::optional<std::pair<std::size_t, Kind>> components(std::uint32_t format) {
    switch (format) {
    case 2: return std::pair<std::size_t, Kind>{4, Kind::kF32};
    case 6: return std::pair<std::size_t, Kind>{3, Kind::kF32};
    case 16: return std::pair<std::size_t, Kind>{2, Kind::kF32};
    case 41: return std::pair<std::size_t, Kind>{1, Kind::kF32};
    case 10: return std::pair<std::size_t, Kind>{4, Kind::kF16};
    case 34: return std::pair<std::size_t, Kind>{2, Kind::kF16};
    case 54: return std::pair<std::size_t, Kind>{1, Kind::kF16};
    case 11: return std::pair<std::size_t, Kind>{4, Kind::kUnorm16};
    case 35: return std::pair<std::size_t, Kind>{2, Kind::kUnorm16};
    case 28:
    case 29:
    case 87:
    case 91: return std::pair<std::size_t, Kind>{4, Kind::kUnorm8};
    default: return std::nullopt;
    }
}

float read(Kind kind, const std::uint8_t* at, std::size_t component) {
    const std::uint8_t* from = at + component * bytes(kind);
    switch (kind) {
    case Kind::kF32: {
        std::uint32_t bits = 0;
        std::memcpy(&bits, from, sizeof bits);
        return std::bit_cast<float>(bits);
    }
    case Kind::kF16: {
        std::uint16_t half = 0;
        std::memcpy(&half, from, sizeof half);
        return half_to_f32(half);
    }
    case Kind::kUnorm16: {
        std::uint16_t value = 0;
        std::memcpy(&value, from, sizeof value);
        return static_cast<float>(value) / 65535.0f;
    }
    case Kind::kUnorm8: return static_cast<float>(*from) / 255.0f;
    }
    return 0.0f;
}

void write(Kind kind, std::uint8_t* at, std::size_t component, float value) {
    std::uint8_t* to = at + component * bytes(kind);
    switch (kind) {
    case Kind::kF32: {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        std::memcpy(to, &bits, sizeof bits);
        break;
    }
    case Kind::kF16: {
        const std::uint16_t half = f32_to_half(value);
        std::memcpy(to, &half, sizeof half);
        break;
    }
    case Kind::kUnorm16: {
        const auto stored =
            static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
        std::memcpy(to, &stored, sizeof stored);
        break;
    }
    case Kind::kUnorm8:
        *to = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        break;
    }
}

}  // namespace

std::uint16_t f32_to_half(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7f'ffffu;
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (exponent <= 0) {
        if (exponent < -10) return sign;
        const std::uint32_t significand = (1u << 23) | mantissa;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        const std::uint32_t halfway = 1u << (shift - 1);
        const std::uint32_t quotient = significand >> shift;
        const std::uint32_t remainder = significand & ((1u << shift) - 1);
        const bool up = remainder > halfway || (remainder == halfway && (quotient & 1) != 0);
        return static_cast<std::uint16_t>(sign | (quotient + (up ? 1u : 0u)));
    }
    const std::uint32_t half = static_cast<std::uint32_t>(exponent) << 10;
    return static_cast<std::uint16_t>(sign + static_cast<std::uint16_t>(half + ((mantissa + 0x1000u) >> 13)));
}

float half_to_f32(std::uint16_t value) {
    const std::uint32_t sign = static_cast<std::uint32_t>(value >> 15) << 31;
    const std::uint32_t exponent = (value >> 10) & 0x1fu;
    const std::uint32_t mantissa = value & 0x3ffu;
    if (exponent == 0) {
        if (mantissa == 0) return std::bit_cast<float>(sign);
        const float scaled = static_cast<float>(mantissa) * std::pow(2.0f, -24.0f);
        return sign != 0 ? -scaled : scaled;
    }
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f80'0000u | mantissa << 13);
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | mantissa << 13);
}

void expand_rects(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination, std::size_t stride,
                  std::span<const std::pair<std::uint32_t, std::uint32_t>> elements) {
    if (stride == 0) return;
    const std::size_t rects = std::min(source.size() / (3 * stride), destination.size() / (6 * stride));
    constexpr std::array<std::size_t, 5> kTriangleVertices{0, 1, 2, 0, 2};
    for (std::size_t rect = 0; rect < rects; ++rect) {
        const auto vertices = source.subspan(rect * 3 * stride, 3 * stride);
        auto out = destination.subspan(rect * 6 * stride, 6 * stride);
        const auto vertex = [&](std::size_t index) { return vertices.subspan(index * stride, stride); };
        for (std::size_t slot = 0; slot < kTriangleVertices.size(); ++slot)
            std::memcpy(out.data() + slot * stride, vertex(kTriangleVertices[slot]).data(), stride);
        std::uint8_t* fourth = out.data() + 5 * stride;
        std::memcpy(fourth, vertex(0).data(), stride);
        for (const auto& [offset, format] : elements) {
            const auto counted = components(format);
            if (!counted) continue;
            const auto [count, kind] = *counted;
            if (offset >= stride) continue;
            const std::size_t inside = (stride - offset) / bytes(kind);
            for (std::size_t component = 0; component < std::min(count, inside); ++component) {
                const auto get = [&](std::size_t index) {
                    return read(kind, vertex(index).data() + offset, component);
                };
                write(kind, fourth + offset, component, get(0) + get(2) - get(1));
            }
        }
    }
}

}  // namespace g6shim::d3d12_xs::primitive
