// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6shim/shader/ir.hpp"

namespace g6shim::shader {

struct Bool {
    std::uint32_t index = 0;

    friend constexpr bool operator==(const Bool&, const Bool&) = default;
    friend constexpr auto operator<=>(const Bool&, const Bool&) = default;
};

inline constexpr Bool kFalse{0};
inline constexpr Bool kTrue{1};

struct BoolHash {
    std::size_t operator()(Bool value) const noexcept { return value.index; }
};

struct Atom {
    enum class Kind : std::uint8_t { Lane, Uniform, Phi, Quad, Opaque, Constant };

    Kind kind = Kind::Opaque;
    Value value;
    std::uint64_t constant = 0;

    static Atom lane(Value value) { return {Kind::Lane, value, 0}; }
    static Atom uniform(Value value) { return {Kind::Uniform, value, 0}; }
    static Atom phi(Value value) { return {Kind::Phi, value, 0}; }
    static Atom quad(Value value) { return {Kind::Quad, value, 0}; }
    static Atom opaque(Value value) { return {Kind::Opaque, value, 0}; }
    static Atom constant_value(std::uint64_t constant) { return {Kind::Constant, Value{}, constant}; }

    friend bool operator==(const Atom&, const Atom&) = default;
};

struct AtomHash {
    std::size_t operator()(const Atom& atom) const noexcept;
};

class Bools {
public:
    static constexpr std::uint32_t kTerminal = 0xFFFFFFFFu;

    Bools();

    std::optional<std::tuple<Atom, Bool, Bool>> node(Bool value) const;
    std::vector<Atom> support(Bool value) const;

    Bool atom(const Atom& atom, std::size_t born);
    Bool mk(std::uint32_t variable, Bool low, Bool high);
    Bool ite(Bool f, Bool g, Bool h);
    Bool and_(Bool a, Bool b);
    Bool or_(Bool a, Bool b);
    Bool not_(Bool a);
    Bool xor_(Bool a, Bool b);
    bool implies(Bool a, Bool b);
    Bool forget(Bool value, std::size_t from, std::unordered_map<Bool, Bool, BoolHash>& memo);
    Bool restrict(Bool value, std::uint32_t variable, bool chosen,
                  std::unordered_map<Bool, Bool, BoolHash>& memo);
    Bool given(Bool value, Bool known);
    bool meet(const std::vector<std::pair<Bool, bool>>& all) const;

    bool overflow() const noexcept { return overflow_; }

private:
    struct Node {
        std::uint32_t variable = kTerminal;
        Bool low;
        Bool high;
    };

    struct NodeKey {
        std::uint32_t variable = 0;
        Bool low;
        Bool high;

        friend bool operator==(const NodeKey&, const NodeKey&) = default;
    };

    struct MemoKey {
        Bool f;
        Bool g;
        Bool h;

        friend bool operator==(const MemoKey&, const MemoKey&) = default;
    };

    struct NodeKeyHash {
        std::size_t operator()(const NodeKey& key) const noexcept;
    };

    struct MemoKeyHash {
        std::size_t operator()(const MemoKey& key) const noexcept;
    };

    std::vector<std::uint32_t> vars(Bool value) const;
    std::pair<Bool, Bool> split(Bool value, std::uint32_t variable) const;
    bool meet_in(std::vector<std::pair<Bool, bool>> all, std::size_t& budget) const;

    std::vector<Node> nodes_;
    std::unordered_map<NodeKey, Bool, NodeKeyHash> unique_;
    std::unordered_map<MemoKey, Bool, MemoKeyHash> memo_;
    std::vector<Atom> atoms_;
    std::vector<std::size_t> born_;
    std::unordered_map<Atom, std::uint32_t, AtomHash> ids_;
    bool overflow_ = false;
};

struct LaneExit {
    enum class Kind : std::uint8_t { Jump, Branch, End, Indirect };

    Kind kind = Kind::End;
    std::size_t target = 0;
    std::size_t not_taken = 0;
    Bool when;

    static LaneExit jump(std::size_t target) { return {Kind::Jump, target, 0, kFalse}; }
    static LaneExit branch(Bool when, std::size_t taken, std::size_t not_taken) {
        return {Kind::Branch, taken, not_taken, when};
    }
    static LaneExit end() { return {Kind::End, 0, 0, kFalse}; }
    static LaneExit indirect() { return {Kind::Indirect, 0, 0, kFalse}; }

    friend bool operator==(const LaneExit&, const LaneExit&) = default;
};

struct Structured {
    Bools bools;
    std::vector<LaneExit> exits;
    std::vector<std::vector<std::optional<Bool>>> guards;
    std::vector<Bool> reach;
    std::unordered_map<Value, Bool, ValueHash> masks;
    std::map<std::pair<Value, Value>, Bool> paired_masks;
    std::unordered_map<Value, Value, ValueHash> highs;
    std::unordered_set<Value, ValueHash> active;
    std::unordered_set<Value, ValueHash> lane_data;

    std::optional<Bool> mask_bits(Value low, std::optional<Value> high) const;
};

Structured structure(const Program& program);

}  // namespace g6shim::shader
