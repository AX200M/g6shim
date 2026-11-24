// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/dxil.hpp"

#include "reader.hpp"

#include <cstring>

namespace g6shim::shader {

namespace {

constexpr std::uint8_t kBitcodeMagic[] = {0x42, 0x43, 0xC0, 0xDE};
constexpr std::size_t kProgramHeader = 24;

bool starts_with(std::span<const std::uint8_t> data, const char* text) {
    const std::size_t length = std::strlen(text);
    if (data.size() < length) return false;
    return std::memcmp(data.data(), text, length) == 0;
}

bool contains(std::span<const std::uint8_t> data, const char* text) {
    const std::size_t length = std::strlen(text);
    if (data.size() < length) return false;
    for (std::size_t i = 0; i + length <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, text, length) == 0) return true;
    }
    return false;
}

}  // namespace

ShaderKind shader_kind_from_raw(std::uint16_t value) {
    ShaderKind kind;
    kind.raw = value;
    switch (value) {
        case 0: kind.id = ShaderKind::Id::Pixel; break;
        case 1: kind.id = ShaderKind::Id::Vertex; break;
        case 2: kind.id = ShaderKind::Id::Geometry; break;
        case 3: kind.id = ShaderKind::Id::Hull; break;
        case 4: kind.id = ShaderKind::Id::Domain; break;
        case 5: kind.id = ShaderKind::Id::Compute; break;
        case 6: kind.id = ShaderKind::Id::Library; break;
        case 7: kind.id = ShaderKind::Id::RayGeneration; break;
        case 8: kind.id = ShaderKind::Id::Intersection; break;
        case 9: kind.id = ShaderKind::Id::AnyHit; break;
        case 10: kind.id = ShaderKind::Id::ClosestHit; break;
        case 11: kind.id = ShaderKind::Id::Miss; break;
        case 12: kind.id = ShaderKind::Id::Callable; break;
        case 13: kind.id = ShaderKind::Id::Mesh; break;
        case 14: kind.id = ShaderKind::Id::Amplification; break;
        case 15: kind.id = ShaderKind::Id::Node; break;
        default: kind.id = ShaderKind::Id::Unknown; break;
    }
    return kind;
}

DxilProgram DxilProgram::parse(std::span<const std::uint8_t> part) {
    const std::uint32_t version = detail::read_u32(part, 0);
    const std::uint64_t size = static_cast<std::uint64_t>(detail::read_u32(part, 4)) * 4;
    if (size > part.size() || size < kProgramHeader)
        detail::fail_malformed("program size disagrees with part size");
    part = part.first(static_cast<std::size_t>(size));
    if (!starts_with(part.subspan(8), "DXIL")) detail::fail_malformed("missing DXIL magic");
    const std::uint32_t dxil_version = detail::read_u32(part, 12);
    const std::size_t offset = detail::read_u32(part, 16);
    const std::size_t length = detail::read_u32(part, 20);
    const std::size_t start = 8 + offset;
    const auto bitcode = detail::bytes(part, start, length);
    if (!bitcode) detail::fail_malformed("bitcode out of bounds");
    if (bitcode->size() < 4 || std::memcmp(bitcode->data(), kBitcodeMagic, 4) != 0)
        detail::fail_malformed("bitcode lacks LLVM magic");
    if (start < kProgramHeader) detail::fail_malformed("bitcode offset too small");
    DxilProgram program;
    program.kind = shader_kind_from_raw(static_cast<std::uint16_t>(version >> 16));
    program.shader_model = {static_cast<std::uint8_t>((version >> 4) & 0xF), static_cast<std::uint8_t>(version & 0xF)};
    program.dxil_version = dxil_version;
    program.bitcode = *bitcode;
    program.prefix = part.subspan(kProgramHeader, start - kProgramHeader);
    program.trailer = part.subspan(start + length);
    return program;
}

XboxExtras DxilProgram::xbox_extras() const { return XboxExtras{starts_with(prefix, "XDAT"), contains(trailer, "xbop")}; }

bool DxilProgram::is_standard_layout() const { return prefix.empty() && trailer.empty(); }

}  // namespace g6shim::shader
