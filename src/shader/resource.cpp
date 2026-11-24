// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/resource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"

namespace g6shim::shader {

namespace {

constexpr std::size_t kMaxRounds = 64;
constexpr std::size_t kMaxJoins = 8;
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

struct TraceError {
    const char* what;
};

struct Term {
    enum class Kind : std::uint8_t { Const, Value };

    Kind kind = Kind::Value;
    std::uint32_t constant = 0;
    Value value;

    static Term constant_value(std::uint32_t constant) { return {Kind::Const, constant, Value{}}; }
    static Term origin(Value value) { return {Kind::Value, 0, value}; }

    friend bool operator==(const Term&, const Term&) = default;
};

struct Sum {
    std::optional<Value> base;
    std::uint32_t offset = 0;
    std::optional<Value> dynamic;
};

struct Moved {
    enum class Kind : std::uint8_t { Const, Value };

    Kind kind = Kind::Const;
    std::uint32_t constant = 0;
    Value value;

    static Moved constant_value(std::uint32_t constant) { return {Kind::Const, constant, Value{}}; }
    static Moved origin(Value value) { return {Kind::Value, 0, value}; }

    friend bool operator==(const Moved&, const Moved&) = default;
};

std::optional<std::uint32_t> lane_of(const Operand& op) {
    if (const auto* c = std::get_if<ConstantOperand>(&op)) return inline_constant(c->code);
    if (const auto* l = std::get_if<LiteralOperand>(&op)) return l->value;
    return std::nullopt;
}

std::uint32_t reverse_bits(std::uint32_t value) {
    value = ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u);
    value = ((value & 0x33333333u) << 2) | ((value >> 2) & 0x33333333u);
    value = ((value & 0x0F0F0F0Fu) << 4) | ((value >> 4) & 0x0F0F0F0Fu);
    value = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    return (value << 16) | (value >> 16);
}

std::optional<Moved> copy_source(const Inst& inst, std::size_t k) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr) return std::nullopt;
    const std::string_view name = inst.name();
    if (name == "s_movk_i32") {
        if (!fields->imm) return std::nullopt;
        return Moved::constant_value(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int16_t>(*fields->imm))));
    }
    if (fields->src.empty()) return std::nullopt;
    const Operand& src = fields->src.front();
    if (name == "s_mov_b32" || name == "s_mov_b64") {
        if (const auto* scalar = std::get_if<ScalarOperand>(&src)) {
            if (scalar->code == kNull) return Moved::constant_value(0);
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) return std::nullopt;
            return Moved::origin(*value);
        }
        if (name == "s_mov_b32") {
            if (const auto* literal = std::get_if<LiteralOperand>(&src))
                return Moved::constant_value(literal->value);
        }
        if (const auto* c = std::get_if<ConstantOperand>(&src)) {
            const auto low = inline_constant(c->code);
            if (!low) return std::nullopt;
            if (name == "s_mov_b32") return Moved::constant_value(*low);
            if (c->code >= 128 && c->code <= 208)
                return Moved::constant_value(k == 0 ? *low
                                                    : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
        }
        return std::nullopt;
    }
    const auto* c = std::get_if<ConstantOperand>(&src);
    if (c == nullptr) return std::nullopt;
    const auto value = inline_constant(c->code);
    if (!value) return std::nullopt;
    if (name == "s_brev_b32") return Moved::constant_value(reverse_bits(*value));
    if (name == "s_not_b32") return Moved::constant_value(~*value);
    return std::nullopt;
}

class Tracer {
public:
    Tracer(const Program& program, const std::vector<RootParam>& layout, HwStage stage)
        : p_(&program), layout_(&layout), stage_(stage) {
        std::size_t n = 0;
        for (const auto& entry : program.inputs)
            n = std::max(n, static_cast<std::size_t>(entry.second.index) + 1);
        for (const Block& block : program.blocks) {
            for (const Phi& phi : block.phis)
                n = std::max(n, static_cast<std::size_t>(phi.value.index) + 1);
            for (const Inst& inst : block.insts)
                for (const auto& entry : inst.writes)
                    n = std::max(n, static_cast<std::size_t>(entry.second.index) + 1);
        }
        defs_.assign(n, std::nullopt);
        phis_.assign(n, std::nullopt);
        inputs_.assign(n, std::nullopt);
        for (const auto& entry : program.inputs) inputs_[entry.second.index] = entry.first;
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            const Block& block = program.blocks[b];
            for (std::size_t i = 0; i < block.phis.size(); ++i) phis_[block.phis[i].value.index] = Site{b, i};
            for (std::size_t i = 0; i < block.insts.size(); ++i)
                for (const auto& entry : block.insts[i].writes) defs_[entry.second.index] = Site{b, i};
        }
        origin_ = propagate_copies(n);
    }

    std::optional<std::pair<std::size_t, std::uint32_t>> param_of(Reg reg) const {
        if (reg.kind != Reg::Kind::S) return std::nullopt;
        for (std::size_t i = 0; i < layout_->size(); ++i) {
            const std::uint8_t first = user_sgpr(stage_, (*layout_)[i].user_data);
            if (reg.code < first) continue;
            const std::uint32_t dword = static_cast<std::uint32_t>(reg.code) - first;
            if (dword < (*layout_)[i].sgprs) return std::pair<std::size_t, std::uint32_t>{i, dword};
        }
        return std::nullopt;
    }

    Address flat_address(const Inst& inst, const Operand* addr, const Operand* saddr, std::int16_t offset) const {
        const auto* vec = addr == nullptr ? nullptr : std::get_if<VectorOperand>(addr);
        if (vec == nullptr) throw TraceError{"a global address not in VGPRs"};
        const std::uint32_t off = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset));
        if (const auto* scalar = saddr == nullptr ? nullptr : std::get_if<ScalarOperand>(saddr);
            scalar != nullptr && scalar->code != kNull) {
            const auto word = [&](std::uint8_t k) -> std::optional<Term> {
                const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
                if (!value) return std::nullopt;
                return origin(*value);
            };
            const auto low = word(0);
            const auto high = word(1);
            if (!low || !high) throw TraceError{"an address not in SGPRs"};
            const auto value = inst.read(Reg::v(vec->reg));
            if (!value) throw TraceError{"a global address"};
            Sum extra = sum(vorigin(*value));
            if (extra.base) throw TraceError{"an offset from a root parameter"};
            extra.offset += off;
            return address(*low, *high, extra);
        }
        const auto word = [&](std::uint8_t k) -> std::optional<Term> {
            const auto value = inst.read(Reg::v(static_cast<std::uint8_t>(vec->reg + k)));
            if (!value) return std::nullopt;
            return vorigin(*value);
        };
        const auto low = word(0);
        const auto high = word(1);
        if (!low || !high) throw TraceError{"a global address"};
        return address(*low, *high, Sum{std::nullopt, off, std::nullopt});
    }

    Address smem_address(const Inst& inst, const Operand& base, std::int32_t offset, const Operand& soffset) const {
        const auto* scalar = std::get_if<ScalarOperand>(&base);
        if (scalar == nullptr) throw TraceError{"an address not in SGPRs"};
        const auto word = [&](std::uint8_t k) -> std::optional<Term> {
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) return std::nullopt;
            return origin(*value);
        };
        const auto low = word(0);
        const auto high = word(1);
        if (!low || !high) throw TraceError{"an address not in SGPRs"};
        const auto so = term(inst, soffset);
        if (!so) throw TraceError{"an unknown offset operand"};
        Sum extra = sum(*so);
        if (extra.base) throw TraceError{"an offset from a root parameter"};
        extra.offset += static_cast<std::uint32_t>(offset);
        return address(*low, *high, extra);
    }

    Source descriptor(const Inst& inst, const Operand& op) const {
        const auto* scalar = std::get_if<ScalarOperand>(&op);
        if (scalar == nullptr) throw TraceError{"a descriptor not in SGPRs"};
        std::vector<Term> words;
        words.reserve(scalar->dwords);
        for (std::uint8_t k = 0; k < scalar->dwords; ++k) {
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) throw TraceError{"a descriptor not in SGPRs"};
            words.push_back(origin(*value));
        }
        std::vector<std::size_t> joins;
        return classify(words, joins);
    }

private:
    struct JoinGuard {
        std::vector<std::size_t>& joins;
        ~JoinGuard() { joins.pop_back(); }
    };

    const Inst& inst_at(Site site) const { return p_->blocks[site.block].insts[site.index]; }

    std::optional<const Inst*> def(Value value) const {
        if (value.index >= defs_.size()) return std::nullopt;
        const auto& site = defs_[value.index];
        if (!site) return std::nullopt;
        return &inst_at(*site);
    }

    std::optional<Site> def_site(Value value) const {
        if (value.index >= defs_.size()) return std::nullopt;
        return defs_[value.index];
    }

    Term origin(Value value) const { return value.index < origin_.size() ? origin_[value.index] : Term::origin(value); }

    std::optional<Term> term(const Inst& inst, const Operand& op) const {
        if (const auto* scalar = std::get_if<ScalarOperand>(&op)) {
            if (scalar->code == kNull) return Term::constant_value(0);
            const auto value = inst.read(Reg::s(scalar->code));
            if (!value) return std::nullopt;
            return origin(*value);
        }
        if (const auto* c = std::get_if<ConstantOperand>(&op)) {
            if (const auto value = inline_constant(c->code)) return Term::constant_value(*value);
            return std::nullopt;
        }
        if (const auto* literal = std::get_if<LiteralOperand>(&op)) return Term::constant_value(literal->value);
        return std::nullopt;
    }

    std::optional<std::pair<const Inst*, std::array<Term, 2>>> sop2(Value value,
                                                                   std::initializer_list<std::string_view> names) const {
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const std::string_view name = (*inst)->name();
        if (std::find(names.begin(), names.end(), name) == names.end()) return std::nullopt;
        if ((*inst)->writes.empty() || (*inst)->writes.front().second != value) return std::nullopt;
        const auto* fields = std::get_if<SopFields>(&(*inst)->fields);
        if (fields == nullptr || fields->src.empty() || fields->src.size() < 2) return std::nullopt;
        const auto first = term(**inst, fields->src[0]);
        const auto second = term(**inst, fields->src[1]);
        if (!first || !second) return std::nullopt;
        return std::pair<const Inst*, std::array<Term, 2>>{*inst, {*first, *second}};
    }

    Sum sum(const Term& value) const {
        if (value.kind == Term::Kind::Const) return Sum{std::nullopt, value.constant, std::nullopt};
        const Value v = value.value;
        const Sum dynamic{std::nullopt, 0, v};
        if (v.index < inputs_.size() && inputs_[v.index]) {
            const auto found = param_of(*inputs_[v.index]);
            if (found && found->second == 0) return Sum{v, 0, std::nullopt};
            return dynamic;
        }
        if (getpc(v)) return Sum{v, 0, std::nullopt};
        std::optional<std::array<Term, 2>> add;
        if (const auto s = sop2(v, {"s_add_u32", "s_add_i32"})) add = s->second;
        if (!add) add = vadd(v);
        if (!add) return dynamic;
        const Sum x = sum((*add)[0]);
        const Sum y = sum((*add)[1]);
        if ((x.base && y.base) || (x.dynamic && y.dynamic)) return dynamic;
        return Sum{x.base ? x.base : y.base, x.offset + y.offset, x.dynamic ? x.dynamic : y.dynamic};
    }

    std::optional<const Inst*> getpc(Value low) const {
        const auto inst = def(low);
        if (!inst) return std::nullopt;
        if ((*inst)->name() != "s_getpc_b64") return std::nullopt;
        if ((*inst)->writes.empty() || (*inst)->writes.front().second != low) return std::nullopt;
        return inst;
    }

    std::optional<std::array<Term, 2>> vadd(Value value) const {
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) return std::nullopt;
        const std::string_view name = (*inst)->name();
        if (name != "v_add_co_u32" && name != "v_add_nc_u32") return std::nullopt;
        if (!fields->dst) return std::nullopt;
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) return std::nullopt;
        if ((*inst)->write(Reg::v(dst->reg)) != std::optional<Value>{value}) return std::nullopt;
        if (fields->abs != 0 || fields->neg != 0 || fields->sdwa || fields->dpp) return std::nullopt;
        if (fields->src.size() < 2) return std::nullopt;
        const auto first = vterm(**inst, fields->src[0]);
        const auto second = vterm(**inst, fields->src[1]);
        if (!first || !second) return std::nullopt;
        return std::array<Term, 2>{*first, *second};
    }

    std::optional<Term> vterm(const Inst& inst, const Operand& op) const {
        if (const auto* vec = std::get_if<VectorOperand>(&op)) {
            const auto value = inst.read(Reg::v(vec->reg));
            if (!value) return std::nullopt;
            return vorigin(*value);
        }
        return term(inst, op);
    }

    Term vorigin(Value value) const {
        const auto inst = def(value);
        if (inst && (*inst)->name() == "v_mov_b32") {
            const auto* fields = std::get_if<Valu>(&(*inst)->fields);
            if (fields != nullptr && fields->abs == 0 && fields->neg == 0 && !fields->sdwa && !fields->dpp &&
                !fields->src.empty()) {
                if (const auto found = vterm(**inst, fields->src.front())) return *found;
            }
        }
        return origin(value);
    }

    Address address(const Term& low, const Term& high, const Sum& extra) const {
        const Sum s = sum(low);
        if (s.dynamic && extra.dynamic) throw TraceError{"an address with two computed terms"};
        if (!s.base) throw TraceError{why_no_base(s.dynamic)};
        const Value base = *s.base;
        if (const auto pc = getpc(base)) {
            std::optional<Value> high_half_value;
            if ((*pc)->writes.size() > 1) high_half_value = (*pc)->writes[1].second;
            if (!high_half(high, high_half_value) &&
                !(high_half_value && carried(high, *high_half_value)))
                throw TraceError{"an address with an untracked high half"};
            return Address{kCode,
                           static_cast<std::uint32_t>((*pc)->machine.offset) + 4u + s.offset + extra.offset,
                           s.dynamic ? s.dynamic : extra.dynamic};
        }
        if (base.index >= inputs_.size() || !inputs_[base.index])
            throw TraceError{"an address not from a root parameter"};
        const auto found = param_of(*inputs_[base.index]);
        if (!found) throw TraceError{"an address not from a root parameter"};
        const std::size_t param = found->first;
        std::optional<Value> high_value;
        switch ((*layout_)[param].root.kind) {
            case RootType::Kind::Table:
            case RootType::Kind::ResourceHeap:
            case RootType::Kind::SamplerHeap: break;
            case RootType::Kind::Descriptor: {
                const Reg reg = Reg::s(static_cast<std::uint8_t>(
                    user_sgpr(stage_, (*layout_)[param].user_data) + 1));
                std::optional<Value> input;
                for (const auto& entry : p_->inputs)
                    if (entry.first == reg) input = entry.second;
                if (!input) throw TraceError{"a root descriptor without its high half"};
                high_value = *input;
                break;
            }
            default: throw TraceError{"an address from a root parameter that is no address"};
        }
        const std::optional<Value> dynamic = s.dynamic ? s.dynamic : extra.dynamic;
        if (!high_half(high, high_value) && !(dynamic && high_value && carried(high, *high_value)))
            throw TraceError{"an address with an untracked high half"};
        return Address{param, s.offset + extra.offset, dynamic};
    }

    bool high_half(const Term& high, std::optional<Value> value) const {
        const Term want = value ? Term::origin(*value) : Term::constant_value(0);
        if (high == want) return true;
        if (high.kind != Term::Kind::Value) return false;
        const auto add = sop2(high.value, {"s_addc_u32"});
        if (!add) return false;
        const Term& a = add->second[0];
        const Term& b = add->second[1];
        return (a == want && b == Term::constant_value(0)) || (b == want && a == Term::constant_value(0));
    }

    bool carried(const Term& high, Value value) const {
        if (high.kind != Term::Kind::Value) return false;
        const Value v = high.value;
        if (v == value) return true;
        if (const auto add = sop2(v, {"s_addc_u32"}))
            return carried(add->second[0], value) || carried(add->second[1], value);
        const auto inst = def(v);
        if (!inst) return false;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) return false;
        if ((*inst)->name() != "v_add_co_ci_u32") return false;
        const std::size_t count = std::min<std::size_t>(2, fields->src.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto t = vterm(**inst, fields->src[i]);
            if (t && carried(*t, value)) return true;
        }
        return false;
    }

    const char* why_no_base(std::optional<Value> dynamic) const {
        if (!dynamic) return "a constant address";
        const Value v = *dynamic;
        if (v.index < inputs_.size() && inputs_[v.index]) return "an address in an SGPR that is no root parameter";
        for (Value leaf : leaves(v)) {
            const auto inst = def(leaf);
            if (inst && (*inst)->name() == "s_getpc_b64") return "an address relative to the program counter";
        }
        const auto inst = def(v);
        if (!inst) return "an address from a phi";
        if ((*inst)->name().starts_with("s_load_")) return "an address loaded from memory";
        return "an address not from a root parameter";
    }

    std::vector<Value> leaves(Value value) const {
        std::vector<Value> out;
        collect_leaves(value, out);
        return out;
    }

    void collect_leaves(Value value, std::vector<Value>& out) const {
        const auto add = sop2(value, {"s_add_u32", "s_add_i32"});
        if (!add) {
            out.push_back(value);
            return;
        }
        for (const Term& t : add->second)
            if (t.kind == Term::Kind::Value) collect_leaves(t.value, out);
    }

    Source classify(const std::vector<Term>& words, std::vector<std::size_t>& joins) const {
        if (const auto source = phi(words, joins)) return *source;
        std::vector<std::uint32_t> constants;
        bool all_constant = true;
        for (const Term& word : words) {
            if (word.kind != Term::Kind::Const) {
                all_constant = false;
                break;
            }
            constants.push_back(word.constant);
        }
        if (all_constant) return Source::constant(constants);
        if (const auto source = loaded(words)) return *source;
        if (words.size() == 4 && words[2].kind == Term::Kind::Const && words[3].kind == Term::Kind::Const) {
            Term high = words[1];
            std::uint32_t bits = 0;
            while (high.kind == Term::Kind::Value) {
                const auto more = set_bits(high.value);
                if (!more) break;
                high = more->first;
                bits |= more->second;
            }
            const Address built = address(words[0], high, Sum{std::nullopt, 0, std::nullopt});
            return Source::built(built, bits, std::array<std::uint32_t, 2>{words[2].constant, words[3].constant});
        }
        const auto is_loaded = [&](const Term& word) {
            if (word.kind != Term::Kind::Value) return false;
            const auto inst = def(word.value);
            return inst && (*inst)->name().starts_with("s_load_");
        };
        for (const Term& word : words)
            if (is_loaded(word)) throw TraceError{"a descriptor changed after loading"};
        throw TraceError{"a descriptor neither loaded nor built"};
    }

    std::optional<Source> phi(const std::vector<Term>& words, std::vector<std::size_t>& joins) const {
        const auto phi_of = [&](const Term& word) -> std::optional<Site> {
            if (word.kind != Term::Kind::Value) return std::nullopt;
            if (word.value.index >= phis_.size()) return std::nullopt;
            return phis_[word.value.index];
        };
        std::optional<std::size_t> block;
        for (const Term& word : words) {
            const auto site = phi_of(word);
            if (!site) continue;
            if (block && *block != site->block) throw TraceError{"a descriptor joined in several blocks"};
            block = site->block;
        }
        if (!block) return std::nullopt;
        if (std::find(joins.begin(), joins.end(), *block) != joins.end())
            throw TraceError{"a descriptor that changes in a loop"};
        if (joins.size() >= kMaxJoins) throw TraceError{"a descriptor joined too often"};
        joins.push_back(*block);
        const JoinGuard guard{joins};
        const std::vector<Phi>& phis = p_->blocks[*block].phis;
        const std::size_t preds = p_->blocks[*block].preds.size();
        std::vector<Source> args;
        args.reserve(preds);
        for (std::size_t j = 0; j < preds; ++j) {
            std::vector<Term> edge;
            edge.reserve(words.size());
            for (const Term& word : words) {
                const auto site = phi_of(word);
                edge.push_back(site ? origin(phis[site->index].args[j]) : word);
            }
            args.push_back(classify(edge, joins));
        }
        return Source::phi(*block, std::move(args));
    }

    std::optional<std::pair<Term, std::uint32_t>> set_bits(Value value) const {
        if (const auto or_ = sop2(value, {"s_or_b32"})) {
            const Term& a = or_->second[0];
            const Term& b = or_->second[1];
            if (b.kind == Term::Kind::Const) return std::pair<Term, std::uint32_t>{a, b.constant};
            if (a.kind == Term::Kind::Const) return std::pair<Term, std::uint32_t>{b, a.constant};
        }
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<SopFields>(&(*inst)->fields);
        if (fields == nullptr || !fields->dst || fields->src.empty()) return std::nullopt;
        const auto* dst = std::get_if<ScalarOperand>(&*fields->dst);
        if (dst == nullptr) return std::nullopt;
        if ((*inst)->name() != "s_bitset1_b32") return std::nullopt;
        const auto bit = term(**inst, fields->src.front());
        if (!bit || bit->kind != Term::Kind::Const) return std::nullopt;
        const auto self = (*inst)->read(Reg::s(dst->code));
        if (!self) return std::nullopt;
        return std::pair<Term, std::uint32_t>{origin(*self),
                                              std::uint32_t{1} << (bit->constant & 31)};
    }

    std::optional<Source> loaded(const std::vector<Term>& words) const {
        if (words.empty() || words[0].kind != Term::Kind::Value) return std::nullopt;
        const Value first_value = words[0].value;
        const auto site = def_site(first_value);
        if (!site) return std::nullopt;
        const Inst& inst = inst_at(*site);
        if (!inst.name().starts_with("s_load_")) return std::nullopt;
        std::size_t first = kNoIndex;
        for (std::size_t i = 0; i < inst.writes.size(); ++i)
            if (inst.writes[i].second == first_value) {
                first = i;
                break;
            }
        if (first == kNoIndex) return std::nullopt;
        for (std::size_t k = 0; k < words.size(); ++k) {
            const std::size_t at = first + k;
            if (at >= inst.writes.size()) return std::nullopt;
            if (words[k] != Term::origin(inst.writes[at].second)) return std::nullopt;
        }
        return Source::loaded(*site, static_cast<std::uint8_t>(first));
    }

    std::optional<Moved> restored(const Inst& inst) const {
        const auto* fields = std::get_if<Valu>(&inst.fields);
        if (fields == nullptr || inst.name() != "v_readlane_b32") return std::nullopt;
        if (fields->src.size() != 2) return std::nullopt;
        const auto* vec = std::get_if<VectorOperand>(&fields->src[0]);
        const auto lane = lane_of(fields->src[1]);
        if (vec == nullptr || !lane) return std::nullopt;
        const auto value = inst.read(Reg::v(vec->reg));
        if (!value) return std::nullopt;
        std::vector<Value> seen;
        try {
            return spilled(*value, *lane, seen);
        } catch (const TraceError&) {
            return std::nullopt;
        }
    }

    std::optional<Moved> spilled(Value value, std::uint32_t lane, std::vector<Value>& seen) const {
        if (value.index < phis_.size() && phis_[value.index]) {
            if (std::find(seen.begin(), seen.end(), value) != seen.end()) return std::nullopt;
            const Site site = *phis_[value.index];
            seen.push_back(value);
            std::optional<Moved> found;
            for (Value arg : p_->blocks[site.block].phis[site.index].args) {
                const auto moved = spilled(arg, lane, seen);
                if (moved) {
                    if (found && !(*found == *moved)) throw TraceError{"spilled lanes disagree"};
                    found = *moved;
                }
            }
            return found;
        }
        const auto inst = def(value);
        if (!inst) throw TraceError{"a spilled value without a definition"};
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) throw TraceError{"a spilled value that is no VGPR write"};
        if ((*inst)->name() != "v_writelane_b32") throw TraceError{"a spilled value that is no lane write"};
        if (!fields->dst || fields->src.size() != 2) throw TraceError{"a lane write with other operands"};
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) throw TraceError{"a lane write outside VGPRs"};
        const auto at = lane_of(fields->src[1]);
        if (!at) throw TraceError{"a lane write to a computed lane"};
        if (*at != lane) {
            const auto previous = (*inst)->read(Reg::v(dst->reg));
            if (!previous) throw TraceError{"a lane write without its old value"};
            return spilled(*previous, lane, seen);
        }
        const Operand& operand = fields->src[0];
        std::optional<Moved> moved;
        if (const auto* scalar = std::get_if<ScalarOperand>(&operand)) {
            if (scalar->code != kNull) {
                const auto read = (*inst)->read(Reg::s(scalar->code));
                if (read) moved = Moved::origin(*read);
            }
        } else if (const auto* literal = std::get_if<LiteralOperand>(&operand)) {
            moved = Moved::constant_value(literal->value);
        } else if (const auto* c = std::get_if<ConstantOperand>(&operand)) {
            if (const auto value_at = inline_constant(c->code)) moved = Moved::constant_value(*value_at);
        }
        if (!moved) throw TraceError{"a spilled value that is no lane value"};
        return moved;
    }

    std::vector<Term> propagate_copies(std::size_t n) const {
        std::vector<std::optional<Term>> origins(n);
        for (const auto& entry : p_->inputs) origins[entry.second.index] = Term::origin(entry.second);
        const std::vector<std::size_t> order = reverse_postorder(p_->blocks);
        for (std::size_t round = 0; round < kMaxRounds; ++round) {
            bool changed = false;
            for (std::size_t b : order) {
                const Block& block = p_->blocks[b];
                for (const Phi& phi : block.phis) {
                    const Term own = Term::origin(phi.value);
                    std::optional<Term> first;
                    bool differ = false;
                    for (Value arg : phi.args) {
                        if (arg.index >= origins.size() || !origins[arg.index]) continue;
                        const Term& t = *origins[arg.index];
                        if (t == own) continue;
                        if (!first)
                            first = t;
                        else if (!(*first == t))
                            differ = true;
                    }
                    if (!first) continue;
                    const Term next = differ ? own : *first;
                    if (!origins[phi.value.index] || !(*origins[phi.value.index] == next)) {
                        origins[phi.value.index] = next;
                        changed = true;
                    }
                }
                for (const Inst& inst : block.insts) {
                    for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                        const Value v = inst.writes[k].second;
                        std::optional<Moved> moved = copy_source(inst, k);
                        if (!moved) moved = restored(inst);
                        std::optional<Term> next;
                        if (moved) {
                            if (moved->kind == Moved::Kind::Const)
                                next = Term::constant_value(moved->constant);
                            else if (moved->value.index < origins.size())
                                next = origins[moved->value.index];
                        } else {
                            next = Term::origin(v);
                        }
                        if (v.index >= origins.size()) continue;
                        if (next && (!origins[v.index] || !(*origins[v.index] == *next))) {
                            origins[v.index] = next;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed) {
                std::vector<Term> out;
                out.reserve(n);
                for (std::size_t v = 0; v < n; ++v)
                    out.push_back(origins[v] ? *origins[v] : Term::origin(Value{static_cast<std::uint32_t>(v)}));
                return out;
            }
        }
        detail::fail_unsupported(0, "copies that do not settle");
        return {};
    }

    const Program* p_;
    const std::vector<RootParam>* layout_;
    HwStage stage_;
    std::vector<std::optional<Site>> defs_;
    std::vector<std::optional<Site>> phis_;
    std::vector<std::optional<Reg>> inputs_;
    std::vector<Term> origin_;
};

}  // namespace

std::optional<std::uint32_t> inline_constant(std::uint8_t code) {
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    if (code >= 128 && code <= 192) return static_cast<std::uint32_t>(code) - 128;
    if (code >= 193 && code <= 208) return static_cast<std::uint32_t>(192 - static_cast<std::int32_t>(code));
    switch (code) {
        case 240: return bits(0.5f);
        case 241: return bits(-0.5f);
        case 242: return bits(1.0f);
        case 243: return bits(-1.0f);
        case 244: return bits(2.0f);
        case 245: return bits(-2.0f);
        case 246: return bits(4.0f);
        case 247: return bits(-4.0f);
        case 248: return 0x3E22F983u;
        default: return std::nullopt;
    }
}

Source Source::loaded(Site load, std::uint8_t first) {
    Source source;
    source.kind = Kind::Loaded;
    source.load = load;
    source.first = first;
    return source;
}

Source Source::built(Address address, std::uint32_t bits, std::array<std::uint32_t, 2> words) {
    Source source;
    source.kind = Kind::Built;
    source.address = address;
    source.bits = bits;
    source.words = words;
    return source;
}

Source Source::constant(std::vector<std::uint32_t> constants) {
    Source source;
    source.kind = Kind::Constant;
    source.constants = std::move(constants);
    return source;
}

Source Source::phi(std::size_t block, std::vector<Source> args) {
    Source source;
    source.kind = Kind::Phi;
    source.block = block;
    source.args = std::move(args);
    return source;
}

std::vector<const Source*> Source::leaves() const {
    if (kind != Kind::Phi) return {this};
    std::vector<const Source*> out;
    for (const Source& arg : args)
        for (const Source* leaf : arg.leaves()) out.push_back(leaf);
    return out;
}

bool operator==(const Source& a, const Source& b) {
    return a.kind == b.kind && a.load == b.load && a.first == b.first && a.address == b.address &&
           a.bits == b.bits && a.words == b.words && a.constants == b.constants && a.block == b.block &&
           a.args == b.args;
}

Resources track(const Program& program, const std::vector<RootParam>& layout, HwStage stage) {
    const Tracer tracer(program, layout, stage);
    Resources out;
    for (const auto& entry : program.inputs) {
        const auto found = tracer.param_of(entry.first);
        if (found) out.params.emplace_back(entry.second, found->first, found->second);
    }
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            const Site site{b, i};
            const std::string_view name = inst.name();
            const auto descriptor = [&](const Operand& op, Role role) {
                out.descriptors.push_back(Descriptor{site, role, tracer.descriptor(inst, op)});
            };
            try {
                if (const auto* image = std::get_if<ImageFields>(&inst.fields)) {
                    descriptor(image->rsrc, Role::Resource);
                    if (image->sampler) descriptor(*image->sampler, Role::Sampler);
                } else if (const auto* buffer = std::get_if<BufferFields>(&inst.fields)) {
                    descriptor(buffer->rsrc, Role::Resource);
                } else if (const auto* smem = std::get_if<SmemFields>(&inst.fields)) {
                    if (name.starts_with("s_buffer_")) {
                        descriptor(smem->base, Role::Resource);
                    } else if (name.find("_load_") != std::string_view::npos ||
                               name.find("_store_") != std::string_view::npos) {
                        out.scalar.emplace_back(
                            site, tracer.smem_address(inst, smem->base, smem->offset, smem->soffset));
                    }
                } else if (const auto* flat = std::get_if<FlatFields>(&inst.fields)) {
                    if (name.starts_with("global_"))
                        out.scalar.emplace_back(site, tracer.flat_address(inst, flat->addr ? &*flat->addr : nullptr,
                                                                          flat->saddr ? &*flat->saddr : nullptr,
                                                                          flat->offset));
                }
            } catch (const TraceError& error) {
                detail::fail_unsupported(inst.machine.offset, error.what);
            }
        }
    }
    return out;
}

}  // namespace g6shim::shader
