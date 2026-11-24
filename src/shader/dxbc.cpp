// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/dxbc.hpp"

#include "reader.hpp"

#include <algorithm>
#include <cstdint>

namespace g6shim::shader {

namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kMaxParts = 256;
constexpr std::size_t kDigestOffset = 4;
constexpr std::size_t kVersionOffset = 0x14;
constexpr std::size_t kTotalSizeOffset = 0x18;
constexpr std::size_t kPartCountOffset = 0x1C;
constexpr std::size_t kPartHeaderSize = 8;
constexpr std::size_t kFourCCSize = 4;

constexpr std::uint8_t kSignature[4] = {'D', 'X', 'B', 'C'};

bool has_dxbc_signature(std::span<const std::uint8_t> data) {
    return data.size() >= kFourCCSize && std::equal(std::begin(kSignature), std::end(kSignature), data.begin());
}

}  // namespace

std::string FourCC::text() const {
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes)
        out.push_back(byte >= 0x21 && byte <= 0x7E ? static_cast<char>(byte) : '.');
    return out;
}

Container Container::parse(std::span<const std::uint8_t> data) {
    if (data.size() < kHeaderSize || !has_dxbc_signature(data)) detail::fail_not_dxbc();
    const detail::Reader reader(data);
    Container container;
    std::copy_n(data.begin() + kDigestOffset, container.digest.size(), container.digest.begin());
    container.version = {reader.u16(kVersionOffset), reader.u16(kVersionOffset + 2)};
    const std::uint64_t total = reader.u32(kTotalSizeOffset);
    if (total > data.size() || total < kHeaderSize)
        detail::fail_malformed("container size disagrees with data");
    const detail::Reader bounded(data.first(static_cast<std::size_t>(total)));
    const std::uint64_t count = bounded.u32(kPartCountOffset);
    if (count > kMaxParts) detail::fail_malformed("implausible part count");
    container.parts.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t offset = bounded.u32(kHeaderSize + kFourCCSize * static_cast<std::size_t>(i));
        const auto header = bounded.at(offset, kPartHeaderSize);
        if (!header) detail::fail_malformed("part header out of bounds");
        Part part;
        std::copy_n(header->begin(), kFourCCSize, part.fourcc.bytes.begin());
        part.offset = static_cast<std::size_t>(offset);
        const std::uint32_t size = bounded.u32(static_cast<std::size_t>(offset) + kFourCCSize);
        const auto body = bounded.at(offset + kPartHeaderSize, size);
        if (!body) detail::fail_malformed("part data out of bounds");
        part.data = *body;
        container.parts.push_back(part);
    }
    return container;
}

const Part* Container::part(FourCC fourcc) const {
    const auto it = std::find_if(parts.begin(), parts.end(), [fourcc](const Part& part) { return part.fourcc == fourcc; });
    return it == parts.end() ? nullptr : &*it;
}

}  // namespace g6shim::shader
