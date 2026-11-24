// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/ir.hpp"

namespace g6shim::shader::emit {

enum class Op : std::uint8_t {
    AddU,
    AddF,
    MinU,
    MaxU,
    MinI,
    MaxI,
    MinF,
    MaxF,
    Or,
    And,
    Xor,
};

std::optional<Op> op_of(std::string_view name);
bool identity_of(Op op, std::uint32_t constant);
bool counts_lanes(Op op);

struct Sym {
    enum class Kind : std::uint8_t { Unknown, K, Own, Uni, Red };

    Kind kind = Kind::Unknown;
    std::uint32_t constant = 0;
    Value value;
    bool has_op = false;
    Op op = Op::AddU;
    Value src;
    Value mask;
    std::uint32_t ident = 0;
    std::uint64_t lanes = 0;

    static Sym unknown() { return {}; }
    static Sym k(std::uint32_t constant) {
        Sym out;
        out.kind = Kind::K;
        out.constant = constant;
        return out;
    }
    static Sym own(Value value) {
        Sym out;
        out.kind = Kind::Own;
        out.value = value;
        return out;
    }
    static Sym uni(Value value) {
        Sym out;
        out.kind = Kind::Uni;
        out.value = value;
        return out;
    }
    static Sym red(std::optional<Op> op, Value src, Value mask, std::uint32_t ident, std::uint64_t lanes) {
        Sym out;
        out.kind = Kind::Red;
        out.has_op = op.has_value();
        out.op = op.value_or(Op::AddU);
        out.src = src;
        out.mask = mask;
        out.ident = ident;
        out.lanes = lanes;
        return out;
    }

    friend bool operator==(const Sym&, const Sym&) = default;
};

struct Shape {
    enum class Kind : std::uint8_t { K, Value, Masked, Active, Prefix };

    Kind kind = Kind::K;
    std::uint32_t constant = 0;
    Value value;
    Value src;
    Value mask;
    std::uint32_t ident = 0;
    Op op = Op::AddU;
    bool inclusive = false;

    static Shape k(std::uint32_t constant) {
        Shape out;
        out.kind = Kind::K;
        out.constant = constant;
        return out;
    }
    static Shape of_value(Value value) {
        Shape out;
        out.kind = Kind::Value;
        out.value = value;
        return out;
    }
    static Shape masked(Value src, Value mask, std::uint32_t ident) {
        Shape out;
        out.kind = Kind::Masked;
        out.src = src;
        out.mask = mask;
        out.ident = ident;
        return out;
    }
    static Shape active(Op op, Value src, Value mask, std::uint32_t ident) {
        Shape out;
        out.kind = Kind::Active;
        out.op = op;
        out.src = src;
        out.mask = mask;
        out.ident = ident;
        return out;
    }
    static Shape prefix(Op op, Value src, Value mask, std::uint32_t ident, bool inclusive) {
        Shape out;
        out.kind = Kind::Prefix;
        out.op = op;
        out.src = src;
        out.mask = mask;
        out.ident = ident;
        out.inclusive = inclusive;
        return out;
    }

    friend bool operator==(const Shape&, const Shape&) = default;
};

struct Section {
    std::size_t end = 0;
    std::unordered_map<Value, Shape, ValueHash> shapes;
};

std::unordered_map<std::size_t, Section> sections(const Program& program, std::size_t block);

std::optional<std::tuple<std::uint32_t, bool, bool>> dpp_lane(const Dpp& dpp, std::uint32_t lane);

std::optional<Shape> per_lane(std::span<const Sym> lanes, std::uint64_t all);

}  // namespace g6shim::shader::emit
