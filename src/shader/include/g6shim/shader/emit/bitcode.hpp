// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace g6shim::shader::emit {

struct Ty {
    std::uint32_t index = 0;

    friend constexpr bool operator==(const Ty&, const Ty&) = default;
};

enum class Bin : std::uint8_t {
    Add = 0,
    Sub = 1,
    Mul = 2,
    UDiv = 3,
    SDiv = 4,
    URem = 5,
    Shl = 7,
    LShr = 8,
    AShr = 9,
    And = 10,
    Or = 11,
    Xor = 12,
};

enum class Rmw : std::uint8_t {
    Xchg = 0,
    Add = 1,
    Sub = 2,
    And = 3,
    Or = 5,
    Xor = 6,
    Max = 7,
    Min = 8,
    UMax = 9,
    UMin = 10,
};

enum class Cast : std::uint8_t {
    Trunc = 0,
    ZExt = 1,
    SExt = 2,
    FpToUi = 3,
    FpToSi = 4,
    UiToFp = 5,
    SiToFp = 6,
    Bitcast = 11,
};

namespace pred {

inline constexpr std::uint8_t kFalse = 0;
inline constexpr std::uint8_t kOeq = 1;
inline constexpr std::uint8_t kOgt = 2;
inline constexpr std::uint8_t kOge = 3;
inline constexpr std::uint8_t kOlt = 4;
inline constexpr std::uint8_t kOle = 5;
inline constexpr std::uint8_t kOne = 6;
inline constexpr std::uint8_t kOrd = 7;
inline constexpr std::uint8_t kUno = 8;
inline constexpr std::uint8_t kUeq = 9;
inline constexpr std::uint8_t kUgt = 10;
inline constexpr std::uint8_t kUge = 11;
inline constexpr std::uint8_t kUlt = 12;
inline constexpr std::uint8_t kUle = 13;
inline constexpr std::uint8_t kUne = 14;
inline constexpr std::uint8_t kTrue = 15;
inline constexpr std::uint8_t kEq = 32;
inline constexpr std::uint8_t kNe = 33;
inline constexpr std::uint8_t kIugt = 34;
inline constexpr std::uint8_t kIuge = 35;
inline constexpr std::uint8_t kIult = 36;
inline constexpr std::uint8_t kIule = 37;
inline constexpr std::uint8_t kSgt = 38;
inline constexpr std::uint8_t kSge = 39;
inline constexpr std::uint8_t kSlt = 40;
inline constexpr std::uint8_t kSle = 41;

}  // namespace pred

struct V {
    enum class Kind : std::uint8_t { Global, Func, Const, Inst };

    Kind kind = Kind::Const;
    std::uint32_t index = 0;

    static constexpr V global(std::uint32_t index) { return {Kind::Global, index}; }
    static constexpr V func(std::uint32_t index) { return {Kind::Func, index}; }
    static constexpr V const_value(std::uint32_t index) { return {Kind::Const, index}; }
    static constexpr V inst(std::uint32_t index) { return {Kind::Inst, index}; }

    friend constexpr bool operator==(const V&, const V&) = default;
};

struct VHash {
    std::size_t operator()(const V& v) const noexcept {
        return static_cast<std::size_t>(v.index) * 2 + static_cast<std::size_t>(v.kind);
    }
};

struct TypeDef {
    enum class Kind : std::uint8_t { Void, Int, Float, Ptr, Struct, Array, Func };

    Kind kind = Kind::Void;
    std::uint32_t bits = 0;
    Ty pointee;
    std::uint32_t space = 0;
    std::optional<std::string> name;
    std::vector<Ty> fields;
    Ty result;

    friend bool operator==(const TypeDef&, const TypeDef&) = default;
};

struct TypeDefHash {
    std::size_t operator()(const TypeDef& def) const noexcept;
};

struct Constant {
    enum class Kind : std::uint8_t { Int, Float, Undef, Aggregate };

    Kind kind = Kind::Int;
    std::int64_t value = 0;
    std::uint32_t bits = 0;
    std::vector<std::uint32_t> ids;

    friend bool operator==(const Constant&, const Constant&) = default;
};

struct ConstantKey {
    Ty type;
    Constant constant;

    friend bool operator==(const ConstantKey&, const ConstantKey&) = default;
};

struct ConstantKeyHash {
    std::size_t operator()(const ConstantKey& key) const noexcept;
};

struct Op {
    enum class Kind : std::uint8_t {
        Bin,
        Cast,
        Cmp,
        Select,
        Extract,
        Call,
        Phi,
        Gep,
        Load,
        Store,
        AtomicRmw,
        CmpXchg,
    };

    Kind kind = Kind::Bin;
    std::uint8_t code = 0;
    Ty type;
    V a;
    V b;
    V c;
    std::uint32_t index = 0;
    std::uint32_t function = 0;
    std::vector<V> args;
    std::vector<std::pair<V, std::size_t>> incoming;

    static Op bin(Bin op, V a, V b);
    static Op cast(Cast op, V v, Ty to);
    static Op cmp(std::uint8_t predicate, V a, V b);
    static Op select(V condition, V on_true, V on_false);
    static Op extract(V aggregate, std::uint32_t index);
    static Op call(std::uint32_t function, std::vector<V> args);
    static Op phi(std::vector<std::pair<V, std::size_t>> incoming);
    static Op gep(Ty type, V pointer, std::vector<V> indices);
    static Op load(V pointer);
    static Op store(V pointer, V value);
    static Op atomic_rmw(Rmw op, V pointer, V value);
    static Op cmp_xchg(V pointer, V expected, V new_value);
};

template <typename F>
void for_each_operand(const Op& op, F&& f) {
    switch (op.kind) {
        case Op::Kind::Bin:
        case Op::Kind::Cmp:
        case Op::Kind::Store:
        case Op::Kind::AtomicRmw:
            f(op.a);
            f(op.b);
            break;
        case Op::Kind::Cast:
        case Op::Kind::Extract:
        case Op::Kind::Load:
            f(op.a);
            break;
        case Op::Kind::Select:
        case Op::Kind::CmpXchg:
            f(op.a);
            f(op.b);
            f(op.c);
            break;
        case Op::Kind::Call:
            for (const V& v : op.args) f(v);
            break;
        case Op::Kind::Phi:
            for (const auto& entry : op.incoming) f(entry.first);
            break;
        case Op::Kind::Gep:
            f(op.a);
            for (const V& v : op.args) f(v);
            break;
    }
}

struct Inst {
    Ty type;
    Op op;
};

struct Term {
    enum class Kind : std::uint8_t { Br, CondBr, Ret };

    Kind kind = Kind::Ret;
    std::size_t target = 0;
    std::size_t not_taken = 0;
    V condition;

    static Term br(std::size_t target) { return {Kind::Br, target, 0, V{}}; }
    static Term cond_br(V condition, std::size_t target, std::size_t not_taken) {
        return {Kind::CondBr, target, not_taken, condition};
    }
    static constexpr Term ret() { return {Kind::Ret, 0, 0, V{}}; }

    friend bool operator==(const Term&, const Term&) = default;
};

struct Block {
    std::vector<std::uint32_t> phis;
    std::vector<std::uint32_t> body;
    std::optional<Term> term;
};

enum class Attr : std::uint8_t {
    ReadNone = 1,
    NoUnwind = 2,
    ReadOnly = 3,
    NoDuplicate = 4,
};

struct Md {
    std::uint32_t index = 0;

    friend constexpr bool operator==(const Md&, const Md&) = default;
};

struct MdDef {
    enum class Kind : std::uint8_t { Str, Value, Node };

    Kind kind = Kind::Str;
    std::string text;
    Ty type;
    V value;
    std::vector<std::optional<Md>> operands;
};

struct Global {
    std::string name;
    Ty type;
    Ty ptr;
    std::uint32_t space = 0;
    std::optional<std::uint32_t> init;
};

struct Func {
    std::string name;
    Ty type;
    std::optional<Attr> attr;
    bool body = false;
};

struct Successors {
    std::array<std::size_t, 2> to{};
    std::size_t count = 0;

    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    std::size_t operator[](std::size_t i) const noexcept { return to[i]; }
    const std::size_t* begin() const noexcept { return to.data(); }
    const std::size_t* end() const noexcept { return to.data() + count; }
    bool contains(std::size_t value) const noexcept;
};

Successors successors(const std::optional<Term>& term);

std::uint64_t signed_vbr(std::int64_t value);

class Writer {
public:
    void enter(std::uint64_t id, std::uint32_t width);
    void exit();
    void record(std::uint64_t code, std::initializer_list<std::uint64_t> operands);
    void record(std::uint64_t code, const std::vector<std::uint64_t>& operands);
    void bytes(std::string_view text);
    void emit(std::uint64_t value, std::uint32_t width);
    void vbr(std::uint64_t value, std::uint32_t width);
    void align();
    std::vector<std::uint8_t> finish();

    std::uint64_t pending() const noexcept { return current_; }

private:
    std::uint32_t width() const noexcept;
    void record_operands(std::uint64_t code, std::span<const std::uint64_t> operands);

    std::vector<std::uint32_t> words_;
    std::uint64_t current_ = 0;
    std::uint32_t bits_ = 0;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack_;
};

class Module {
public:
    Ty ty(TypeDef def);

    Ty void_type() { return ty(TypeDef{TypeDef::Kind::Void}); }
    Ty int_type(std::uint32_t bits) {
        TypeDef def;
        def.kind = TypeDef::Kind::Int;
        def.bits = bits;
        return ty(std::move(def));
    }
    Ty float_type() { return ty(TypeDef{TypeDef::Kind::Float}); }
    Ty ptr(Ty to);
    Ty ptr_in(Ty to, std::uint32_t space);
    Ty array(std::uint32_t count, Ty of);
    Ty named_struct(const std::string& name, std::vector<Ty> fields);
    Ty func_ty(Ty result, std::vector<Ty> params);

    V int_const(Ty type, std::int64_t value);
    V i32(std::uint32_t value);
    V i8(std::uint8_t value);
    V boolean(bool value);
    V f32(std::uint32_t bits);
    V undef(Ty type);
    V aggregate(Ty type, std::span<const V> fields);
    void replace_const(V old_value, V new_value);

    V function(const std::string& name, Ty type, std::optional<Attr> attr, bool body);
    V global(const std::string& name, Ty type, std::uint32_t space);
    V constant_global(const std::string& name, V init);

    std::size_t new_block();
    Ty type_of(V value) const;

    V bin(std::size_t block, Bin op, V a, V b);
    V cast(std::size_t block, Cast op, V value, Ty to);
    V cmp(std::size_t block, std::uint8_t predicate, V a, V b);
    V select(std::size_t block, V condition, V on_true, V on_false);
    V extract(std::size_t block, V aggregate, std::uint32_t index);
    V call(std::size_t block, V function, std::vector<V> args);
    V phi(std::size_t block, Ty type);
    V element(std::size_t block, V pointer, V index);
    V load(std::size_t block, V pointer);
    void store(std::size_t block, V pointer, V value);
    V atomic(std::size_t block, Rmw op, V pointer, V value);
    V cmpxchg(std::size_t block, V pointer, V expected, V new_value);

    void set_phi(V phi, std::vector<std::pair<V, std::size_t>> incoming);
    void terminate(std::size_t block, Term term);

    Md md_str(const std::string& text);
    Md md_value(V value);
    Md md_node(std::vector<std::optional<Md>> operands);
    void named_md(const std::string& name, std::vector<Md> nodes);

    const std::vector<Inst>& instructions() const noexcept { return insts_; }
    const std::vector<Block>& blocks() const noexcept { return blocks_; }

    std::vector<bool> live() const;
    std::vector<std::uint8_t> write() const;

private:
    V constant(Ty type, Constant constant);
    V push(std::size_t block, Ty type, Op op);
    bool is_void(Ty type) const;

    std::vector<TypeDef> types_;
    std::unordered_map<TypeDef, std::uint32_t, TypeDefHash> type_ids_;
    std::vector<ConstantKey> consts_;
    std::unordered_map<ConstantKey, std::uint32_t, ConstantKeyHash> const_ids_;
    std::vector<Global> globals_;
    std::vector<Func> funcs_;
    std::unordered_map<std::string, std::uint32_t> func_ids_;
    std::vector<Inst> insts_;
    std::vector<Block> blocks_;
    std::vector<MdDef> mds_;
    std::vector<std::pair<std::string, std::vector<Md>>> named_;
};

}  // namespace g6shim::shader::emit
