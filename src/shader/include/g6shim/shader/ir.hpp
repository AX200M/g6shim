// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "g6shim/shader/gfx10.hpp"

namespace g6shim::shader {

struct Reg {
    enum class Kind : std::uint8_t { S, Scc, V };

    Kind kind = Kind::S;
    std::uint8_t code = 0;

    static constexpr Reg s(std::uint8_t code) { return {Kind::S, code}; }
    static constexpr Reg scc() { return {Kind::Scc, 0}; }
    static constexpr Reg v(std::uint8_t code) { return {Kind::V, code}; }

    friend constexpr bool operator==(const Reg&, const Reg&) = default;
    friend constexpr auto operator<=>(const Reg&, const Reg&) = default;
};

inline constexpr std::uint8_t kM0 = 124;

struct Value {
    std::uint32_t index = 0;

    friend constexpr bool operator==(const Value&, const Value&) = default;
    friend constexpr auto operator<=>(const Value&, const Value&) = default;
};

struct ValueHash {
    std::size_t operator()(Value value) const noexcept { return value.index; }
};

struct Exit {
    enum class Kind : std::uint8_t { Jump, Branch, End, Indirect };

    Kind kind = Kind::End;
    std::size_t target = 0;
    std::size_t not_taken = 0;

    static Exit jump(std::size_t target) { return {Kind::Jump, target, 0}; }
    static Exit branch(std::size_t taken, std::size_t not_taken) { return {Kind::Branch, taken, not_taken}; }
    static Exit end() { return {Kind::End, 0, 0}; }
    static Exit indirect() { return {Kind::Indirect, 0, 0}; }

    std::vector<std::size_t> successors() const;

    friend bool operator==(const Exit&, const Exit&) = default;
};

struct Phi {
    Reg reg;
    Value value;
    std::vector<Value> args;

    friend bool operator==(const Phi&, const Phi&) = default;
};

struct Inst {
    Instruction machine;
    Fields fields;
    std::vector<std::pair<Reg, Value>> reads;
    std::vector<std::pair<Reg, Value>> writes;

    std::string_view name() const;
    std::optional<Value> read(Reg reg) const;
    std::optional<Value> write(Reg reg) const;
};

struct Block {
    std::size_t offset = 0;
    std::vector<std::size_t> preds;
    std::vector<Phi> phis;
    std::vector<Inst> insts;
    Exit exit;

    friend bool operator==(const Block&, const Block&) = default;
};

struct Program {
    bool wave64 = false;
    std::uint16_t vgprs = 0;
    std::vector<Block> blocks;
    std::vector<std::pair<Reg, Value>> inputs;

    friend bool operator==(const Program&, const Program&) = default;
};

Program build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs);

std::vector<std::size_t> reverse_postorder(const std::vector<Block>& blocks);

std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(const std::vector<Block>& blocks,
                                                                   const std::vector<std::size_t>& order,
                                                                   const std::vector<std::size_t>& index);

std::vector<std::size_t> idoms(const std::vector<Block>& blocks,
                               const std::vector<std::size_t>& order,
                               const std::vector<std::size_t>& index);

}  // namespace g6shim::shader
