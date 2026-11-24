// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6shim/shader/gfx10_fields.hpp"

namespace g6shim::shader {

enum class Encoding : std::uint8_t {
    Sop2,
    Sopk,
    Sop1,
    Sopc,
    Sopp,
    Smem,
    Vop2,
    Vop1,
    Vopc,
    Vop3,
    Vop3p,
    Vintrp,
    Ds,
    Mubuf,
    Mtbuf,
    Mimg,
    Exp,
    Flat,
    Global,
    Scratch,
};

inline constexpr Encoding kEncodings[] = {
    Encoding::Sop2, Encoding::Sopk, Encoding::Sop1,    Encoding::Sopc,  Encoding::Sopp,
    Encoding::Smem, Encoding::Vop2, Encoding::Vop1,    Encoding::Vopc,  Encoding::Vop3,
    Encoding::Vop3p, Encoding::Vintrp, Encoding::Ds,   Encoding::Mubuf, Encoding::Mtbuf,
    Encoding::Mimg, Encoding::Exp,  Encoding::Flat,    Encoding::Global, Encoding::Scratch,
};

struct OpcodeEntry {
    std::uint16_t code = 0;
    const char* name = "";
};

struct Instruction {
    std::size_t offset = 0;
    Encoding encoding = Encoding::Vop1;
    std::uint16_t opcode = 0;
    std::vector<std::uint32_t> words;

    std::optional<std::string_view> name() const;
    Fields fields(bool wave64) const;
    Valu valu(std::string_view name, std::uint8_t lanes) const;
    std::string text(bool wave64) const;

    friend bool operator==(const Instruction&, const Instruction&) = default;
};

std::optional<std::string_view> opcode_name(Encoding encoding, std::uint16_t opcode);
std::span<const OpcodeEntry> opcode_table(Encoding encoding);

std::vector<Instruction> decode(std::span<const std::uint8_t> code);

}  // namespace g6shim::shader
