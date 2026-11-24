// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace g6shim::shader {

inline constexpr std::uint8_t kNull = 125;
inline constexpr std::uint8_t kVcc = 106;
inline constexpr std::uint8_t kExec = 126;

struct ScalarOperand {
    std::uint8_t code = 0;
    std::uint8_t dwords = 0;

    friend bool operator==(const ScalarOperand&, const ScalarOperand&) = default;
};

struct VectorOperand {
    std::uint8_t reg = 0;
    std::uint8_t dwords = 0;

    friend bool operator==(const VectorOperand&, const VectorOperand&) = default;
};

struct ConstantOperand {
    std::uint8_t code = 0;

    friend bool operator==(const ConstantOperand&, const ConstantOperand&) = default;
};

struct LiteralOperand {
    std::uint32_t value = 0;

    friend bool operator==(const LiteralOperand&, const LiteralOperand&) = default;
};

struct AttributeOperand {
    std::uint8_t index = 0;
    std::uint8_t chan = 0;
    bool high = false;

    friend bool operator==(const AttributeOperand&, const AttributeOperand&) = default;
};

using Operand = std::variant<ScalarOperand, VectorOperand, ConstantOperand, LiteralOperand, AttributeOperand>;

struct Cache {
    bool glc = false;
    bool slc = false;
    bool dlc = false;

    friend bool operator==(const Cache&, const Cache&) = default;
};

struct Sdwa {
    std::uint8_t dst_sel = 0;
    std::uint8_t dst_unused = 0;
    std::array<std::uint8_t, 2> src_sel{};
    std::uint8_t sext = 0;

    friend bool operator==(const Sdwa&, const Sdwa&) = default;
};

struct DppRow {
    std::uint16_t ctrl = 0;
    std::uint8_t row_mask = 0;
    std::uint8_t bank_mask = 0;
    bool bound_ctrl = false;
    bool fi = false;

    friend bool operator==(const DppRow&, const DppRow&) = default;
};

struct DppLanes {
    std::uint32_t sel = 0;
    bool fi = false;

    friend bool operator==(const DppLanes&, const DppLanes&) = default;
};

using Dpp = std::variant<DppRow, DppLanes>;

struct Valu {
    std::optional<Operand> dst;
    std::optional<Operand> carry;
    std::vector<Operand> src;
    std::uint8_t neg = 0;
    std::uint8_t abs = 0;
    std::uint8_t neg_hi = 0;
    std::uint8_t opsel = 0;
    std::uint8_t opsel_hi = 0;
    bool clamp = false;
    std::uint8_t omod = 0;
    std::optional<Sdwa> sdwa;
    std::optional<Dpp> dpp;

    friend bool operator==(const Valu&, const Valu&) = default;
};

struct SopFields {
    std::optional<Operand> dst;
    std::vector<Operand> src;
    std::optional<std::uint16_t> imm;

    friend bool operator==(const SopFields&, const SopFields&) = default;
};

struct SoppFields {
    std::uint16_t imm = 0;

    friend bool operator==(const SoppFields&, const SoppFields&) = default;
};

struct SmemFields {
    std::optional<Operand> data;
    Operand base;
    std::int32_t offset = 0;
    Operand soffset;
    Cache cache;

    friend bool operator==(const SmemFields&, const SmemFields&) = default;
};

struct VintrpFields {
    Operand dst;
    Operand src;
    Operand attr;

    friend bool operator==(const VintrpFields&, const VintrpFields&) = default;
};

struct DsFields {
    std::optional<Operand> dst;
    std::optional<Operand> addr;
    std::vector<Operand> data;
    std::uint8_t offset0 = 0;
    std::uint8_t offset1 = 0;
    bool gds = false;

    friend bool operator==(const DsFields&, const DsFields&) = default;
};

struct BufferFields {
    std::optional<Operand> data;
    Operand addr;
    Operand rsrc;
    Operand soffset;
    std::uint16_t offset = 0;
    bool offen = false;
    bool idxen = false;
    std::optional<std::uint8_t> format;
    bool lds = false;
    bool tfe = false;
    Cache cache;

    friend bool operator==(const BufferFields&, const BufferFields&) = default;
};

struct ImageFields {
    Operand data;
    std::vector<Operand> addr;
    Operand rsrc;
    std::optional<Operand> sampler;
    std::uint8_t dmask = 0;
    std::uint8_t dim = 0;
    bool unorm = false;
    bool r128 = false;
    bool a16 = false;
    bool d16 = false;
    bool tfe = false;
    bool lwe = false;
    Cache cache;

    friend bool operator==(const ImageFields&, const ImageFields&) = default;
};

struct FlatFields {
    std::optional<Operand> dst;
    std::optional<Operand> addr;
    std::optional<Operand> data;
    std::optional<Operand> saddr;
    std::int16_t offset = 0;
    bool lds = false;
    Cache cache;

    friend bool operator==(const FlatFields&, const FlatFields&) = default;
};

struct ExpFields {
    std::uint8_t target = 0;
    std::array<std::optional<Operand>, 4> src;
    bool compr = false;
    bool done = false;
    bool vm = false;

    friend bool operator==(const ExpFields&, const ExpFields&) = default;
};

using Fields = std::variant<SopFields,
                            SoppFields,
                            SmemFields,
                            Valu,
                            VintrpFields,
                            DsFields,
                            BufferFields,
                            ImageFields,
                            FlatFields,
                            ExpFields>;

bool ds_pair(std::string_view name);

}  // namespace g6shim::shader
