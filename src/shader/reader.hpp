// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "g6shim/shader/error.hpp"

namespace g6shim::shader::detail {

inline std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 2) fail_malformed("truncated");
    return static_cast<std::uint16_t>(data[at] | data[at + 1] << 8);
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 4) fail_malformed("truncated");
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}

inline std::uint64_t read_u64(std::span<const std::uint8_t> data, std::size_t at) {
    const std::uint64_t low = read_u32(data, at);
    const std::uint64_t high = read_u32(data, at + 4);
    return low | high << 32;
}

inline std::optional<std::span<const std::uint8_t>> bytes(std::span<const std::uint8_t> data,
                                                          std::uint64_t offset,
                                                          std::uint64_t len) {
    if (offset > data.size() || len > data.size() - offset) return std::nullopt;
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(len));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::span<const std::uint8_t> data() const { return data_; }

    std::uint16_t u16(std::size_t at) const { return read_u16(data_, at); }
    std::uint32_t u32(std::size_t at) const { return read_u32(data_, at); }
    std::uint64_t u64(std::size_t at) const { return read_u64(data_, at); }

    std::optional<std::span<const std::uint8_t>> at(std::uint64_t offset, std::uint64_t len) const {
        return bytes(data_, offset, len);
    }

    std::span<const std::uint8_t> slice(std::size_t offset, std::size_t len) const {
        if (offset > data_.size() || len > data_.size() - offset) fail_malformed("truncated");
        return data_.subspan(offset, len);
    }

private:
    std::span<const std::uint8_t> data_;
};

}  // namespace g6shim::shader::detail
