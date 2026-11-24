// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/structure.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/ir.hpp"

namespace g6shim::shader {

namespace {

constexpr std::size_t kMaxNodes = 1u << 21;
constexpr int kMaxRounds = 256;
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

std::size_t shifted(std::uint64_t value) {
    value = (value ^ (value >> 33)) * 0xFF51AFD7ED558CCDu;
    value = (value ^ (value >> 33)) * 0xC4CEB9FE1A85EC53u;
    return static_cast<std::size_t>(value ^ (value >> 33));
}

std::size_t mix(std::uint64_t seed, std::uint64_t value) {
    return shifted(seed ^ (value + 0x9E3779B97F4A7C15u + (seed << 6) + (seed >> 2)));
}

std::string_view strip_suffix(std::string_view text, std::string_view suffix) {
    if (!text.ends_with(suffix)) return {};
    return text.substr(0, text.size() - suffix.size());
}

bool ends_with_suffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

using Slot = std::pair<std::size_t, std::size_t>;

struct MaskKey {
    Value low;
    Value high;

    friend bool operator==(const MaskKey&, const MaskKey&) = default;
};

struct MaskKeyHash {
    std::size_t operator()(const MaskKey& key) const noexcept {
        return mix(key.low.index, key.high.index);
    }
};

struct SopMasks {
    std::optional<std::pair<MaskKey, Bool>> dst;
    std::optional<std::pair<MaskKey, Bool>> exec;
    bool derived = false;
    std::optional<Bool> scc;
};

struct Join {
    std::size_t block = 0;
    Value id;
    bool value = false;
    std::vector<MaskKey> args;
};

class Lanes {
public:
    explicit Lanes(const Program& program);

    void reset();
    std::vector<std::vector<std::optional<Bool>>> masks_in_order();
    std::vector<Bool> paths();
    std::vector<Value> wrong_joins(const std::vector<Bool>& cond);
    void mark_unresolved(const std::vector<Value>& wrong);
    Structured finish(const std::vector<std::vector<std::optional<Bool>>>& execs, const std::vector<Bool>& cond);

private:
    template <typename Lookup>
    std::optional<MaskKey> key(Lookup lookup, std::uint8_t code) const {
        const auto low = lookup(Reg::s(code));
        if (!low) return std::nullopt;
        if (!p_->wave64) return MaskKey{*low, *low};
        const auto high = lookup(Reg::s(static_cast<std::uint8_t>(code + 1)));
        if (!high) return std::nullopt;
        return MaskKey{*low, *high};
    }

    const Inst& inst_at(Slot site) const { return p_->blocks[site.first].insts[site.second]; }
    std::optional<Slot> def_site(Value value) const;
    std::optional<Slot> phi_site(Value value) const;
    std::size_t born(Value value) const;
    std::optional<Bool> cached(MaskKey key) const;
    bool exec_write(std::optional<Slot> site, Value value) const;

    Bool edge(Bool c, std::size_t q, bool taken);
    Bool exec(const Inst& inst);
    std::pair<Bool, bool> mask(MaskKey key);
    std::optional<std::pair<Bool, bool>> join(MaskKey key);
    std::optional<std::pair<Bool, bool>> operand(const Inst& inst, const Operand& op);
    std::optional<SopMasks> sop(const Inst& inst);
    std::optional<Bool> combine(std::string_view f, Bool a, Bool c);
    void step(const Inst& inst, std::optional<Bool> exec);
    Bool branch(const Inst& inst);

    const Program* p_;
    std::vector<std::size_t> order_;
    std::vector<std::size_t> index_;
    std::vector<std::vector<std::pair<std::size_t, bool>>> incoming_;
    std::unordered_map<Value, Slot, ValueHash> defs_;
    std::unordered_map<Value, Slot, ValueHash> phis_;
    std::unordered_set<Value, ValueHash> unresolved_;
    Bools b_;
    std::unordered_map<MaskKey, Bool, MaskKeyHash> masks_;
    std::unordered_map<MaskKey, Bool, MaskKeyHash> other_;
    std::unordered_map<Value, Bool, ValueHash> scc_lane_;
    std::vector<Join> joins_;
    std::vector<Bool> first_;
    std::vector<std::optional<Bool>> when_;
    std::unordered_set<Value, ValueHash> active_;
};

void check_reducible(const Program& program, const std::vector<std::size_t>& order,
                     const std::vector<std::size_t>& index) {
    const auto edge = irreducible_edge(program.blocks, order, index);
    if (edge) detail::fail_unsupported(program.blocks[edge->first].offset, "irreducible control flow");
}

}  // namespace

std::size_t AtomHash::operator()(const Atom& atom) const noexcept {
    std::size_t seed = mix(static_cast<std::uint64_t>(atom.kind), atom.value.index);
    return mix(seed, atom.constant);
}

Bools::Bools() {
    nodes_.push_back(Node{kTerminal, kFalse, kFalse});
    nodes_.push_back(Node{kTerminal, kTrue, kTrue});
}

std::size_t Bools::NodeKeyHash::operator()(const NodeKey& key) const noexcept {
    const std::size_t seed = mix(key.variable, key.low.index);
    return mix(seed, key.high.index);
}

std::size_t Bools::MemoKeyHash::operator()(const MemoKey& key) const noexcept {
    const std::size_t seed = mix(key.f.index, key.g.index);
    return mix(seed, key.h.index);
}

std::optional<std::tuple<Atom, Bool, Bool>> Bools::node(Bool value) const {
    const Node entry = nodes_[value.index];
    if (entry.variable == kTerminal) return std::nullopt;
    return std::tuple<Atom, Bool, Bool>{atoms_[entry.variable], entry.low, entry.high};
}

std::vector<Atom> Bools::support(Bool value) const {
    std::vector<std::uint32_t> variables = vars(value);
    std::sort(variables.begin(), variables.end());
    std::vector<Atom> out;
    out.reserve(variables.size());
    for (std::uint32_t variable : variables) out.push_back(atoms_[variable]);
    return out;
}

Bool Bools::atom(const Atom& value, std::size_t born) {
    std::uint32_t variable;
    if (const auto it = ids_.find(value); it != ids_.end()) {
        variable = it->second;
    } else {
        atoms_.push_back(value);
        born_.push_back(born);
        variable = static_cast<std::uint32_t>(atoms_.size() - 1);
        ids_.emplace(value, variable);
    }
    return mk(variable, kFalse, kTrue);
}

Bool Bools::mk(std::uint32_t variable, Bool low, Bool high) {
    if (low == high) return low;
    const NodeKey key{variable, low, high};
    if (const auto it = unique_.find(key); it != unique_.end()) return it->second;
    if (nodes_.size() >= kMaxNodes) {
        overflow_ = true;
        return kFalse;
    }
    const Bool value{static_cast<std::uint32_t>(nodes_.size())};
    nodes_.push_back(Node{variable, low, high});
    unique_.emplace(key, value);
    return value;
}

std::pair<Bool, Bool> Bools::split(Bool value, std::uint32_t variable) const {
    const Node entry = nodes_[value.index];
    if (entry.variable == variable) return {entry.low, entry.high};
    return {value, value};
}

Bool Bools::ite(Bool f, Bool g, Bool h) {
    if (f == kTrue || g == h) return g;
    if (f == kFalse) return h;
    if (g == kTrue && h == kFalse) return f;
    const MemoKey key{f, g, h};
    if (const auto it = memo_.find(key); it != memo_.end()) return it->second;
    const std::uint32_t variable = std::min({nodes_[f.index].variable, nodes_[g.index].variable,
                                             nodes_[h.index].variable});
    const auto [f0, f1] = split(f, variable);
    const auto [g0, g1] = split(g, variable);
    const auto [h0, h1] = split(h, variable);
    const Bool low = ite(f0, g0, h0);
    const Bool high = ite(f1, g1, h1);
    const Bool result = mk(variable, low, high);
    memo_.emplace(key, result);
    return result;
}

Bool Bools::and_(Bool a, Bool b) { return ite(a, b, kFalse); }

Bool Bools::or_(Bool a, Bool b) { return ite(a, kTrue, b); }

Bool Bools::not_(Bool a) { return ite(a, kFalse, kTrue); }

Bool Bools::xor_(Bool a, Bool b) { return ite(a, not_(b), b); }

bool Bools::implies(Bool a, Bool b) { return and_(a, not_(b)) == kFalse; }

Bool Bools::forget(Bool value, std::size_t from, std::unordered_map<Bool, Bool, BoolHash>& memo) {
    const Node entry = nodes_[value.index];
    if (entry.variable == kTerminal) return value;
    if (const auto it = memo.find(value); it != memo.end()) return it->second;
    const Bool low = forget(entry.low, from, memo);
    const Bool high = forget(entry.high, from, memo);
    const Bool result = born_[entry.variable] >= from ? or_(low, high) : mk(entry.variable, low, high);
    memo.emplace(value, result);
    return result;
}

Bool Bools::restrict(Bool value, std::uint32_t variable, bool chosen,
                     std::unordered_map<Bool, Bool, BoolHash>& memo) {
    const Node entry = nodes_[value.index];
    if (entry.variable == kTerminal || entry.variable > variable) return value;
    if (entry.variable == variable) return chosen ? entry.high : entry.low;
    if (const auto it = memo.find(value); it != memo.end()) return it->second;
    const Bool low = restrict(entry.low, variable, chosen, memo);
    const Bool high = restrict(entry.high, variable, chosen, memo);
    const Bool result = mk(entry.variable, low, high);
    memo.emplace(value, result);
    return result;
}

Bool Bools::given(Bool value, Bool known) {
    for (std::uint32_t variable : vars(value)) {
        const Bool x = mk(variable, kFalse, kTrue);
        const Bool nx = not_(x);
        std::unordered_map<Bool, Bool, BoolHash> memo;
        if (implies(known, x)) {
            value = restrict(value, variable, true, memo);
        } else if (implies(known, nx)) {
            value = restrict(value, variable, false, memo);
        }
    }
    return value;
}

std::vector<std::uint32_t> Bools::vars(Bool value) const {
    std::vector<Bool> seen;
    std::vector<std::uint32_t> variables;
    std::vector<Bool> stack{value};
    while (!stack.empty()) {
        const Bool current = stack.back();
        stack.pop_back();
        const Node entry = nodes_[current.index];
        if (entry.variable == kTerminal || std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
        seen.push_back(current);
        if (std::find(variables.begin(), variables.end(), entry.variable) == variables.end())
            variables.push_back(entry.variable);
        stack.push_back(entry.low);
        stack.push_back(entry.high);
    }
    return variables;
}

Lanes::Lanes(const Program& program) : p_(&program) {
    const std::vector<Block>& blocks = program.blocks;
    order_ = reverse_postorder(blocks);
    index_.assign(blocks.size(), kNoIndex);
    for (std::size_t i = 0; i < order_.size(); ++i) index_[order_[i]] = i;
    check_reducible(program, order_, index_);
    incoming_.assign(blocks.size(), {});
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const std::vector<std::size_t> successors = blocks[b].exit.successors();
        for (std::size_t k = 0; k < successors.size(); ++k) incoming_[successors[k]].emplace_back(b, k == 0);
        for (std::size_t i = 0; i < blocks[b].phis.size(); ++i) phis_.emplace(blocks[b].phis[i].value, Slot{b, i});
        for (std::size_t i = 0; i < blocks[b].insts.size(); ++i)
            for (const auto& entry : blocks[b].insts[i].writes) defs_.emplace(entry.second, Slot{b, i});
    }
    first_.assign(blocks.size(), kFalse);
    when_.assign(blocks.size(), std::nullopt);
}

void Lanes::reset() {
    b_ = Bools{};
    masks_.clear();
    other_.clear();
    scc_lane_.clear();
    joins_.clear();
    std::fill(first_.begin(), first_.end(), kFalse);
    std::fill(when_.begin(), when_.end(), std::nullopt);
    active_.clear();
}

std::optional<Slot> Lanes::def_site(Value value) const {
    const auto it = defs_.find(value);
    if (it == defs_.end()) return std::nullopt;
    return it->second;
}

std::optional<Slot> Lanes::phi_site(Value value) const {
    const auto it = phis_.find(value);
    if (it == phis_.end()) return std::nullopt;
    return it->second;
}

std::size_t Lanes::born(Value value) const {
    if (const auto def = def_site(value)) return index_[def->first];
    if (const auto phi = phi_site(value)) return index_[phi->first];
    return 0;
}

std::optional<Bool> Lanes::cached(MaskKey key) const {
    if (const auto it = masks_.find(key); it != masks_.end()) return it->second;
    if (const auto it = other_.find(key); it != other_.end()) return it->second;
    return std::nullopt;
}

bool Lanes::exec_write(std::optional<Slot> site, Value value) const {
    if (!site) return false;
    const Inst& inst = inst_at(*site);
    if (inst.write(Reg::s(kExec)) == std::optional<Value>{value}) return true;
    return inst.write(Reg::s(static_cast<std::uint8_t>(kExec + 1))) == std::optional<Value>{value};
}

Bool Lanes::edge(Bool c, std::size_t q, bool taken) {
    const std::optional<Bool>& when = when_[q];
    if (!when) return c;
    if (taken) return b_.and_(c, *when);
    return b_.and_(c, b_.not_(*when));
}

std::vector<std::vector<std::optional<Bool>>> Lanes::masks_in_order() {
    const Program& p = *p_;
    const auto input = [&](Reg reg) -> std::optional<Value> {
        for (const auto& entry : p.inputs)
            if (entry.first == reg) return entry.second;
        return std::nullopt;
    };
    if (const auto low = input(Reg::s(kExec))) {
        const Value high = p.wave64 ? input(Reg::s(static_cast<std::uint8_t>(kExec + 1))).value_or(*low) : *low;
        masks_.emplace(MaskKey{*low, high}, kTrue);
        active_.insert(*low);
    }
    std::vector<std::vector<std::optional<Bool>>> execs(p.blocks.size());
    for (std::size_t n = 0; n < p.blocks.size(); ++n) execs[n].assign(p.blocks[n].insts.size(), std::nullopt);
    for (std::size_t n = 0; n < order_.size(); ++n) {
        const std::size_t b = order_[n];
        Bool c = n == 0 ? kTrue : kFalse;
        for (std::size_t k = 0; k < incoming_[b].size(); ++k) {
            const auto [q, taken] = incoming_[b][k];
            if (index_[q] < n) c = b_.or_(c, edge(first_[q], q, taken));
        }
        first_[b] = c;
        for (const Phi& phi : p.blocks[b].phis)
            if (phi.reg == Reg::s(kExec)) active_.insert(phi.value);
        std::optional<Bool> now;
        for (std::size_t i = 0; i < p.blocks[b].insts.size(); ++i) {
            const Inst& inst = p.blocks[b].insts[i];
            if (inst.read(Reg::s(kExec))) {
                const Bool e = exec(inst);
                execs[b][i] = e;
                now = e;
            }
            step(inst, now);
            if (const auto k = key([&](Reg r) { return inst.write(r); }, kExec)) now = cached(*k);
        }
        if (p.blocks[b].exit.kind == Exit::Kind::Branch && !p.blocks[b].insts.empty())
            when_[b] = branch(p.blocks[b].insts.back());
    }
    return execs;
}

Bool Lanes::exec(const Inst& inst) {
    if (const auto k = key([&](Reg r) { return inst.read(r); }, kExec)) return mask(*k).first;
    return kTrue;
}

std::pair<Bool, bool> Lanes::mask(MaskKey key) {
    if (const auto it = masks_.find(key); it != masks_.end()) return {it->second, true};
    if (const auto it = other_.find(key); it != other_.end()) return {it->second, false};
    std::pair<Bool, bool> result;
    if (const auto found = join(key)) {
        result = *found;
    } else {
        const auto low = def_site(key.low);
        const auto high = def_site(key.high);
        std::optional<Bool> m;
        if (low && high && *low == *high) {
            const auto sop_result = sop(inst_at(*low));
            if (sop_result && sop_result->dst) m = sop_result->dst->second;
        }
        const bool derived = exec_write(low, key.low) || exec_write(high, key.high);
        if (!m) m = b_.atom(Atom::opaque(key.low), born(key.low));
        result = {*m, derived};
    }
    if (result.second)
        masks_.insert_or_assign(key, result.first);
    else
        other_.insert_or_assign(key, result.first);
    return result;
}

std::optional<std::pair<Bool, bool>> Lanes::join(MaskKey key) {
    const auto low = phi_site(key.low);
    const auto high = phi_site(key.high);
    std::size_t block = 0;
    if (low && high) {
        if (low->first != high->first) return std::nullopt;
        block = low->first;
    } else if (low) {
        block = low->first;
    } else if (high) {
        block = high->first;
    } else {
        return std::nullopt;
    }
    const Block& target = p_->blocks[block];
    const auto arg = [&](std::optional<Slot> phi, Value own, std::size_t n) {
        return phi ? target.phis[phi->second].args[n] : own;
    };
    std::vector<MaskKey> args;
    args.reserve(target.preds.size());
    for (std::size_t n = 0; n < target.preds.size(); ++n)
        args.push_back(MaskKey{arg(low, key.low, n), arg(high, key.high, n)});
    const Value id = low ? key.low : key.high;
    bool derived = false;
    std::vector<std::pair<Bool, Bool>> forward;
    for (std::size_t n = 0; n < args.size(); ++n) {
        const auto [q, taken] = incoming_[block][n];
        if (index_[q] < index_[block]) {
            const auto [m, d] = mask(args[n]);
            derived = derived || d;
            forward.emplace_back(edge(first_[q], q, taken), m);
        }
    }
    if (forward.size() == args.size() && !forward.empty()) {
        const Bool m = forward.front().second;
        bool same = true;
        for (const auto& entry : forward)
            if (entry.second != m) same = false;
        if (same) return std::pair<Bool, bool>{m, derived};
    }
    if (unresolved_.find(id) == unresolved_.end()) {
        for (bool value : {true, false}) {
            bool holds = true;
            for (const auto& entry : forward) {
                const Bool m = value ? entry.second : b_.not_(entry.second);
                if (!b_.implies(entry.first, m)) {
                    holds = false;
                    break;
                }
            }
            if (holds) {
                joins_.push_back(Join{block, id, value, args});
                return std::pair<Bool, bool>{value ? kTrue : kFalse, derived};
            }
        }
    }
    return std::pair<Bool, bool>{b_.atom(Atom::phi(id), index_[block]), derived};
}

std::optional<std::pair<Bool, bool>> Lanes::operand(const Inst& inst, const Operand& op) {
    const auto constant = [&](std::uint64_t value) {
        if (!p_->wave64) value &= 0xFFFFFFFFull;
        if (value == 0) return kFalse;
        const std::uint64_t all = p_->wave64 ? 0xFFFFFFFFFFFFFFFFull : 0xFFFFFFFFull;
        if (value == all) return kTrue;
        return b_.atom(Atom::constant_value(value), 0);
    };
    if (const auto* scalar = std::get_if<ScalarOperand>(&op)) {
        if (scalar->code == kNull) return std::nullopt;
        const auto k = key([&](Reg r) { return inst.read(r); }, scalar->code);
        if (!k) return std::nullopt;
        return mask(*k);
    }
    if (const auto* literal = std::get_if<LiteralOperand>(&op))
        return std::pair<Bool, bool>{constant(literal->value), false};
    if (const auto* c = std::get_if<ConstantOperand>(&op)) {
        if (c->code < 128 || c->code > 208) return std::nullopt;
        const std::int64_t value = c->code <= 192 ? static_cast<std::int64_t>(c->code) - 128
                                                  : 192 - static_cast<std::int64_t>(c->code);
        return std::pair<Bool, bool>{constant(static_cast<std::uint64_t>(value)), false};
    }
    return std::nullopt;
}

std::optional<SopMasks> Lanes::sop(const Inst& inst) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr) return std::nullopt;
    const std::string_view width = p_->wave64 ? "_b64" : "_b32";
    const std::string_view name = inst.name();
    if (!name.starts_with("s_") || !ends_with_suffix(name, width)) return std::nullopt;
    std::string_view op = name.substr(2);
    op = op.substr(0, op.size() - width.size());
    std::optional<MaskKey> dst;
    if (fields->dst) {
        const auto* scalar = std::get_if<ScalarOperand>(&*fields->dst);
        if (scalar != nullptr && scalar->code != kNull)
            dst = key([&](Reg r) { return inst.write(r); }, scalar->code);
    }
    const auto exec_out = key([&](Reg r) { return inst.write(r); }, kExec);
    std::optional<bool> save;
    std::string_view f = op;
    if (ends_with_suffix(op, "_saveexec")) {
        save = true;
        f = strip_suffix(op, "_saveexec");
    } else if (ends_with_suffix(op, "_wrexec")) {
        save = false;
        f = strip_suffix(op, "_wrexec");
    }
    bool derived = exec_out.has_value() || save.has_value();
    const auto arg = [&](std::size_t n) -> std::optional<Bool> {
        if (n >= fields->src.size()) return std::nullopt;
        const auto m = operand(inst, fields->src[n]);
        if (!m) return std::nullopt;
        derived = derived || m->second;
        return m->first;
    };
    if (save) {
        const auto s = arg(0);
        if (!s) return std::nullopt;
        const Bool exec_now = exec(inst);
        const auto next = combine(f, *s, exec_now);
        if (!next) return std::nullopt;
        const Bool saved = *save ? exec_now : *next;
        SopMasks result;
        if (dst) result.dst = std::pair<MaskKey, Bool>{*dst, saved};
        if (exec_out) result.exec = std::pair<MaskKey, Bool>{*exec_out, *next};
        result.derived = derived;
        result.scc = *next;
        return result;
    }
    Bool value;
    std::optional<Bool> scc;
    if (f == "mov") {
        const auto a = arg(0);
        if (!a) return std::nullopt;
        value = *a;
    } else if (f == "not") {
        const auto a = arg(0);
        if (!a) return std::nullopt;
        value = b_.not_(*a);
        scc = value;
    } else if (f == "wqm") {
        const auto a = arg(0);
        if (!a) return std::nullopt;
        const std::optional<MaskKey> low = dst ? dst : exec_out;
        if (!low) return std::nullopt;
        const Bool quad = b_.atom(Atom::quad(low->low), born(low->low));
        value = b_.or_(*a, quad);
        scc = value;
    } else if (f == "cselect") {
        const auto a = arg(0);
        if (!a) return std::nullopt;
        const auto c = arg(1);
        if (!c) return std::nullopt;
        const auto scc_value = inst.read(Reg::scc());
        if (!scc_value) return std::nullopt;
        const Bool uniform = b_.atom(Atom::uniform(*scc_value), born(*scc_value));
        value = b_.ite(uniform, *a, *c);
    } else {
        const auto a = arg(0);
        if (!a) return std::nullopt;
        const auto c = arg(1);
        if (!c) return std::nullopt;
        const auto next = combine(f, *a, *c);
        if (!next) return std::nullopt;
        value = *next;
        scc = value;
    }
    SopMasks result;
    if (dst) result.dst = std::pair<MaskKey, Bool>{*dst, value};
    if (exec_out) result.exec = std::pair<MaskKey, Bool>{*exec_out, value};
    result.derived = derived;
    result.scc = scc;
    return result;
}

std::optional<Bool> Lanes::combine(std::string_view f, Bool a, Bool c) {
    if (f == "and") return b_.and_(a, c);
    if (f == "or") return b_.or_(a, c);
    if (f == "xor") return b_.xor_(a, c);
    if (f == "andn2") return b_.and_(a, b_.not_(c));
    if (f == "orn2") return b_.or_(a, b_.not_(c));
    if (f == "andn1") return b_.and_(b_.not_(a), c);
    if (f == "orn1") return b_.or_(b_.not_(a), c);
    if (f == "nand") return b_.not_(b_.and_(a, c));
    if (f == "nor") return b_.not_(b_.or_(a, c));
    if (f == "xnor") return b_.not_(b_.xor_(a, c));
    return std::nullopt;
}

void Lanes::step(const Inst& inst, std::optional<Bool> exec_mask) {
    const std::string_view name = inst.name();
    if (const auto* fields = std::get_if<SopFields>(&inst.fields)) {
        bool takes_mask = false;
        for (const Operand& s : fields->src) {
            const auto* scalar = std::get_if<ScalarOperand>(&s);
            if (scalar == nullptr || scalar->code == kNull) continue;
            const auto k = key([&](Reg r) { return inst.read(r); }, scalar->code);
            if (!k) continue;
            const bool phi = phis_.find(k->low) != phis_.end() || phis_.find(k->high) != phis_.end();
            takes_mask = takes_mask || masks_.find(*k) != masks_.end() || (phi && mask(*k).second);
        }
        const bool touches_exec =
            inst.write(Reg::s(kExec)).has_value() || inst.read(Reg::s(kExec)).has_value();
        if (takes_mask || touches_exec) {
            const auto result = sop(inst);
            if (result && result->derived) {
                const auto note = [&](const std::pair<MaskKey, Bool>& entry) {
                    masks_.insert_or_assign(entry.first, entry.second);
                    if (result->exec && result->exec->first == entry.first) {
                        active_.insert(entry.first.low);
                    } else if (exec_mask && b_.implies(entry.second, *exec_mask)) {
                        active_.insert(entry.first.low);
                    }
                };
                if (result->dst) note(*result->dst);
                if (result->exec) note(*result->exec);
                if (const auto scc = inst.write(Reg::scc()); scc && result->scc)
                    scc_lane_.insert_or_assign(*scc, *result->scc);
            }
        }
        const std::string_view width = p_->wave64 ? "64" : "32";
        if (takes_mask && name.starts_with("s_cmp_")) {
            const auto scc = inst.write(Reg::scc());
            const std::string_view rest = name.substr(6);
            if (scc && ends_with_suffix(rest, width)) {
                const std::string_view kind = rest.substr(0, rest.size() - width.size());
                if ((kind == "eq_u" || kind == "lg_u") && fields->src.size() == 2) {
                    const auto a = operand(inst, fields->src[0]);
                    const auto c = operand(inst, fields->src[1]);
                    if (a && c) {
                        const Bool ne = b_.xor_(a->first, c->first);
                        const Bool m = kind.starts_with("eq") ? b_.not_(ne) : ne;
                        scc_lane_.insert_or_assign(*scc, m);
                    }
                }
            }
        }
    } else if (const auto* val = std::get_if<Valu>(&inst.fields)) {
        const Bool current = exec(inst);
        const auto lane = [&](MaskKey k) {
            const Bool a = b_.atom(Atom::lane(k.low), born(k.low));
            const Bool m = b_.and_(current, a);
            masks_.insert_or_assign(k, m);
            active_.insert(k.low);
        };
        if (name == "v_cndmask_b32" && val->src.size() > 2) {
            const auto* scalar = std::get_if<ScalarOperand>(&val->src[2]);
            if (scalar != nullptr && scalar->code != kNull) {
                if (const auto k = key([&](Reg r) { return inst.read(r); }, scalar->code)) mask(*k);
            }
        }
        if (name.starts_with("v_cmpx")) {
            if (const auto k = key([&](Reg r) { return inst.write(r); }, kExec)) lane(*k);
        } else {
            std::vector<Operand> compare;
            if (name.starts_with("v_cmp") && val->dst) compare.push_back(*val->dst);
            if (val->carry) compare.push_back(*val->carry);
            for (const Operand& op : compare) {
                const auto* scalar = std::get_if<ScalarOperand>(&op);
                if (scalar == nullptr) continue;
                if (const auto k = key([&](Reg r) { return inst.write(r); }, scalar->code)) lane(*k);
            }
        }
    }
}

Bool Lanes::branch(const Inst& inst) {
    const Bool current = exec(inst);
    const std::string_view name = inst.name();
    if (name == "s_cbranch_execz") return b_.not_(current);
    if (name == "s_cbranch_execnz") return current;
    if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
        Bool vcc = kFalse;
        if (const auto k = key([&](Reg r) { return inst.read(r); }, kVcc)) vcc = mask(*k).first;
        const Bool any = b_.and_(vcc, current);
        if (name.ends_with("nz")) return any;
        return b_.not_(any);
    }
    const auto scc = inst.read(Reg::scc());
    if (!scc) return kFalse;
    Bool set;
    if (const auto it = scc_lane_.find(*scc); it != scc_lane_.end())
        set = it->second;
    else
        set = b_.atom(Atom::uniform(*scc), born(*scc));
    if (name == "s_cbranch_scc1") return set;
    return b_.not_(set);
}

std::vector<Bool> Lanes::paths() {
    const std::size_t n = p_->blocks.size();
    std::vector<Bool> cond(n, kFalse);
    if (!order_.empty()) cond[order_.front()] = kTrue;
    for (int round = 0; round < kMaxRounds; ++round) {
        bool changed = false;
        for (std::size_t i = 1; i < order_.size(); ++i) {
            const std::size_t s = order_[i];
            Bool c = kFalse;
            for (std::size_t k = 0; k < incoming_[s].size(); ++k) {
                const auto [q, taken] = incoming_[s][k];
                Bool e = edge(cond[q], q, taken);
                if (i <= index_[q]) {
                    std::unordered_map<Bool, Bool, BoolHash> memo;
                    e = b_.forget(e, i, memo);
                }
                c = b_.or_(c, e);
            }
            if (c != cond[s]) {
                cond[s] = c;
                changed = true;
            }
        }
        if (b_.overflow()) detail::fail_isa(0, "lane conditions too large");
        if (!changed) return cond;
    }
    detail::fail_isa(0, "lane conditions do not settle");
    return {};
}

std::vector<Value> Lanes::wrong_joins(const std::vector<Bool>& cond) {
    std::vector<Value> wrong;
    std::size_t j = 0;
    while (j < joins_.size()) {
        const std::size_t block = joins_[j].block;
        const Value id = joins_[j].id;
        const bool value = joins_[j].value;
        const std::vector<MaskKey> args = joins_[j].args;
        for (std::size_t n = 0; n < args.size(); ++n) {
            const auto [q, taken] = incoming_[block][n];
            const Bool e = edge(cond[q], q, taken);
            Bool m = mask(args[n]).first;
            if (!value) m = b_.not_(m);
            if (!b_.implies(e, m)) {
                wrong.push_back(id);
                break;
            }
        }
        ++j;
    }
    return wrong;
}

void Lanes::mark_unresolved(const std::vector<Value>& wrong) {
    for (Value value : wrong) unresolved_.insert(value);
}

Structured Lanes::finish(const std::vector<std::vector<std::optional<Bool>>>& execs,
                         const std::vector<Bool>& cond) {
    const std::vector<Block>& blocks = p_->blocks;
    std::vector<std::vector<std::optional<Bool>>> guards;
    guards.reserve(blocks.size());
    for (std::size_t n = 0; n < blocks.size(); ++n) {
        std::vector<std::optional<Bool>> row;
        row.reserve(blocks[n].insts.size());
        for (std::size_t i = 0; i < blocks[n].insts.size(); ++i) {
            const std::optional<Bool>& exec_mask = execs[n][i];
            const Fields& fields = blocks[n].insts[i].fields;
            const bool scalar = std::holds_alternative<SopFields>(fields) ||
                                std::holds_alternative<SoppFields>(fields) ||
                                std::holds_alternative<SmemFields>(fields);
            std::optional<Bool> guard;
            if (exec_mask && !scalar && !b_.implies(cond[n], *exec_mask))
                guard = b_.given(*exec_mask, cond[n]);
            row.push_back(guard);
        }
        guards.push_back(std::move(row));
    }
    std::vector<LaneExit> exits;
    exits.reserve(blocks.size());
    for (std::size_t n = 0; n < blocks.size(); ++n) {
        const Exit& exit = blocks[n].exit;
        switch (exit.kind) {
            case Exit::Kind::Jump: exits.push_back(LaneExit::jump(exit.target)); break;
            case Exit::Kind::End: exits.push_back(LaneExit::end()); break;
            case Exit::Kind::Indirect: exits.push_back(LaneExit::indirect()); break;
            case Exit::Kind::Branch:
                exits.push_back(
                    LaneExit::branch(b_.given(when_[n].value_or(kFalse), cond[n]), exit.target, exit.not_taken));
                break;
        }
    }
    if (b_.overflow()) detail::fail_isa(0, "lane conditions too large");
    Structured result;
    for (const auto& entry : masks_) result.masks.emplace(entry.first.low, entry.second);
    for (const auto& entry : masks_)
        if (entry.first.low != entry.first.high) result.highs.emplace(entry.first.high, entry.first.low);
    result.active = std::move(active_);
    result.bools = std::move(b_);
    result.exits = std::move(exits);
    result.guards = std::move(guards);
    return result;
}

Structured structure(const Program& program) {
    Lanes lanes(program);
    for (;;) {
        const auto execs = lanes.masks_in_order();
        const auto cond = lanes.paths();
        const auto wrong = lanes.wrong_joins(cond);
        if (wrong.empty()) return lanes.finish(execs, cond);
        lanes.mark_unresolved(wrong);
        lanes.reset();
    }
}

}  // namespace g6shim::shader
