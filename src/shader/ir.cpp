// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/ir.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"

namespace g6shim::shader {

namespace {

constexpr std::uint32_t kNoValue = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kNoBlock = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kRegs = 128 + 1 + 256;
constexpr std::size_t kSetWords = (kRegs + 63) / 64;

using RegSet = std::array<std::uint64_t, kSetWords>;

[[noreturn]] void fail_unsupported_name(std::size_t offset, std::string_view what) {
    const std::string text(what);
    detail::fail_unsupported(offset, text.c_str());
}

Operand vector(std::uint8_t reg, std::uint8_t dwords) { return Operand{VectorOperand{reg, dwords}}; }

std::string_view second_token(std::string_view name) {
    const std::size_t first = name.find('_');
    if (first == std::string_view::npos) return {};
    const std::size_t second = name.find('_', first + 1);
    return name.substr(first + 1, second == std::string_view::npos ? std::string_view::npos : second - (first + 1));
}

std::size_t reg_index(Reg reg) {
    switch (reg.kind) {
        case Reg::Kind::S: return reg.code;
        case Reg::Kind::Scc: return 128;
        case Reg::Kind::V: return 129 + reg.code;
    }
    return 0;
}

Reg reg_at(std::size_t index) {
    if (index < 128) return Reg::s(static_cast<std::uint8_t>(index));
    if (index == 128) return Reg::scc();
    return Reg::v(static_cast<std::uint8_t>(index - 129));
}

void insert(RegSet& set, Reg reg) {
    const std::size_t index = reg_index(reg);
    set[index / 64] |= std::uint64_t{1} << (index % 64);
}

bool contains(const RegSet& set, Reg reg) {
    const std::size_t index = reg_index(reg);
    return ((set[index / 64] >> (index % 64)) & 1) != 0;
}

std::vector<Reg> members(const RegSet& set) {
    std::vector<Reg> out;
    for (std::size_t index = 0; index < kRegs; ++index)
        if (((set[index / 64] >> (index % 64)) & 1) != 0) out.push_back(reg_at(index));
    return out;
}

Value lookup(const std::vector<std::pair<Reg, Value>>& outs, Reg reg) {
    const auto it = std::lower_bound(outs.begin(), outs.end(), reg,
                                     [](const std::pair<Reg, Value>& entry, Reg key) { return entry.first < key; });
    if (it == outs.end() || it->first != reg) return Value{kNoValue};
    return it->second;
}

void link(std::vector<Block>& blocks) {
    for (Block& block : blocks) block.preds.clear();
    for (std::size_t b = 0; b < blocks.size(); ++b)
        for (std::size_t s : blocks[b].exit.successors()) blocks[s].preds.push_back(b);
}

std::optional<std::pair<std::vector<bool>, std::vector<std::size_t>>> multi_entry_loop(
    const std::vector<Block>& blocks, const std::vector<bool>& inside) {
    const auto reach = [&](std::size_t from, bool forward) {
        std::vector<bool> seen(blocks.size(), false);
        std::vector<std::size_t> stack{from};
        while (!stack.empty()) {
            const std::size_t b = stack.back();
            stack.pop_back();
            const std::vector<std::size_t> next = forward ? blocks[b].exit.successors() : blocks[b].preds;
            for (std::size_t n : next)
                if (inside[n] && !seen[n]) {
                    seen[n] = true;
                    stack.push_back(n);
                }
        }
        return seen;
    };
    std::vector<bool> done(blocks.size(), false);
    for (std::size_t v = 0; v < blocks.size(); ++v) {
        if (!inside[v] || done[v]) continue;
        const std::vector<bool> ahead = reach(v, true);
        const std::vector<bool> behind = reach(v, false);
        std::vector<bool> region(blocks.size(), false);
        for (std::size_t i = 0; i < blocks.size(); ++i) region[i] = ahead[i] && behind[i];
        if (!region[v]) continue;
        std::vector<std::size_t> contained;
        for (std::size_t b = 0; b < blocks.size(); ++b)
            if (region[b]) contained.push_back(b);
        for (std::size_t b : contained) done[b] = true;
        std::vector<std::size_t> entries;
        for (std::size_t b : contained)
            for (std::size_t p : blocks[b].preds)
                if (!region[p]) {
                    entries.push_back(b);
                    break;
                }
        if (entries.empty()) continue;
        if (entries.size() == 1) {
            std::vector<bool> nested = region;
            nested[entries[0]] = false;
            if (auto found = multi_entry_loop(blocks, nested)) return found;
            continue;
        }
        return std::pair<std::vector<bool>, std::vector<std::size_t>>{std::move(region), std::move(entries)};
    }
    return std::nullopt;
}

void split_irreducible(std::vector<Block>& blocks) {
    const std::size_t limit = 4 * blocks.size() + 64;
    for (;;) {
        const std::vector<std::size_t> order = reverse_postorder(blocks);
        std::vector<std::size_t> index(blocks.size(), kNoBlock);
        for (std::size_t i = 0; i < order.size(); ++i) index[order[i]] = i;
        const auto edge = irreducible_edge(blocks, order, index);
        if (!edge) return;
        const std::size_t from = edge->first;
        const std::vector<bool> all(blocks.size(), true);
        auto found = multi_entry_loop(blocks, all);
        if (!found || blocks.size() >= limit) fail_unsupported_name(blocks[from].offset, "irreducible control flow");
        std::vector<bool> region = std::move(found->first);
        std::vector<std::size_t> entries = std::move(found->second);
        std::sort(entries.begin(), entries.end(),
                  [&](std::size_t a, std::size_t b) { return index[a] < index[b]; });
        const std::size_t header = entries[0];
        const std::size_t entry = entries[1];

        std::vector<std::size_t> copy(blocks.size(), kNoBlock);
        std::vector<std::size_t> copied;
        std::vector<std::size_t> stack{entry};
        while (!stack.empty()) {
            const std::size_t b = stack.back();
            stack.pop_back();
            if (copy[b] != kNoBlock) continue;
            copy[b] = blocks.size() + copied.size();
            copied.push_back(b);
            for (std::size_t s : blocks[b].exit.successors())
                if (region[s] && s != header) stack.push_back(s);
        }
        const auto retarget = [](Exit& exit, const auto& to) {
            if (exit.kind == Exit::Kind::Jump) {
                exit.target = to(exit.target);
            } else if (exit.kind == Exit::Kind::Branch) {
                exit.target = to(exit.target);
                exit.not_taken = to(exit.not_taken);
            }
        };
        for (std::size_t b : copied) {
            Block block = blocks[b];
            retarget(block.exit, [&](std::size_t s) { return copy[s] == kNoBlock ? s : copy[s]; });
            blocks.push_back(std::move(block));
        }
        std::vector<std::size_t> outside;
        for (std::size_t p : blocks[entry].preds)
            if (!region[p]) outside.push_back(p);
        for (std::size_t p : outside)
            retarget(blocks[p].exit, [&](std::size_t s) { return s == entry ? copy[entry] : s; });
        link(blocks);
    }
}

std::pair<std::vector<RegSet>, std::vector<RegSet>> liveness(const std::vector<Block>& blocks,
                                                            const std::vector<std::size_t>& order) {
    const RegSet empty{};
    std::vector<RegSet> uses(blocks.size(), empty);
    std::vector<RegSet> defs(blocks.size(), empty);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        for (const Inst& inst : blocks[b].insts) {
            for (const auto& entry : inst.reads)
                if (!contains(defs[b], entry.first)) insert(uses[b], entry.first);
            for (const auto& entry : inst.writes) insert(defs[b], entry.first);
        }
    }
    std::vector<RegSet> live_in(blocks.size(), empty);
    std::vector<RegSet> live_out(blocks.size(), empty);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const std::size_t b = *it;
            RegSet out{};
            for (std::size_t s : blocks[b].exit.successors())
                for (std::size_t k = 0; k < kSetWords; ++k) out[k] |= live_in[s][k];
            RegSet in{};
            for (std::size_t k = 0; k < kSetWords; ++k) in[k] = uses[b][k] | (out[k] & ~defs[b][k]);
            if (in != live_in[b] || out != live_out[b]) {
                live_in[b] = in;
                live_out[b] = out;
                changed = true;
            }
        }
    }
    return {std::move(live_in), std::move(live_out)};
}

bool dropped(std::string_view name) {
    static constexpr std::array<std::string_view, 19> kDropped{
        "s_nop",          "s_waitcnt",         "s_waitcnt_vscnt",   "s_waitcnt_vmcnt",
        "s_waitcnt_expcnt", "s_waitcnt_lgkmcnt", "s_waitcnt_depctr", "s_wait_idle",
        "s_inst_prefetch", "s_clause",          "s_setprio",         "s_sleep",
        "s_ttracedata",    "s_ttracedata_imm",  "s_incperflevel",    "s_decperflevel",
        "v_nop",           "v_pipeflush",       "v_clrexcp",
    };
    return std::find(kDropped.begin(), kDropped.end(), name) != kDropped.end();
}

bool writes_scc(Encoding encoding, std::string_view name) {
    if (encoding == Encoding::Sopc) return true;
    if (encoding == Encoding::Sopk) return name.starts_with("s_cmpk_") || name == "s_addk_i32";
    static constexpr std::array<std::string_view, 29> kStems{
        "add",   "sub",   "addc", "subb", "min",   "max", "and",   "or",     "xor",    "andn2",
        "orn2",  "nand",  "nor",  "xnor", "lshl",  "lshr", "ashr", "bfe",    "absdiff", "lshl1",
        "lshl2", "lshl3", "lshl4", "not", "wqm",  "bcnt0", "bcnt1", "quadmask", "abs",
    };
    if (name.find("saveexec") != std::string_view::npos || name.find("wrexec") != std::string_view::npos) return true;
    const std::string_view stem = second_token(name);
    return std::find(kStems.begin(), kStems.end(), stem) != kStems.end();
}

Operand atomic_result(const Operand& data, std::string_view name) {
    const auto* vec = std::get_if<VectorOperand>(&data);
    if (vec != nullptr && name.find("cmpswap") != std::string_view::npos)
        return vector(vec->reg, static_cast<std::uint8_t>(vec->dwords / 2));
    return data;
}

bool add_regs(const Operand& op, std::uint8_t lanes, std::vector<Reg>& out) {
    if (const auto* s = std::get_if<ScalarOperand>(&op)) {
        if (s->code == kNull) return true;
        const std::uint16_t end = static_cast<std::uint16_t>(s->code) + s->dwords;
        if (end > 128) return false;
        for (std::uint16_t code = s->code; code < end; ++code) out.push_back(Reg::s(static_cast<std::uint8_t>(code)));
        return true;
    }
    if (const auto* v = std::get_if<VectorOperand>(&op)) {
        if (static_cast<std::uint16_t>(v->reg) + v->dwords > 256) return false;
        for (std::uint8_t i = 0; i < v->dwords; ++i) out.push_back(Reg::v(static_cast<std::uint8_t>(v->reg + i)));
        return true;
    }
    if (const auto* c = std::get_if<ConstantOperand>(&op)) {
        switch (c->code) {
            case 251:
                for (std::uint8_t i = 0; i < lanes; ++i)
                    out.push_back(Reg::s(static_cast<std::uint8_t>(kVcc + i)));
                break;
            case 252:
                for (std::uint8_t i = 0; i < lanes; ++i)
                    out.push_back(Reg::s(static_cast<std::uint8_t>(kExec + i)));
                break;
            case 253: out.push_back(Reg::scc()); break;
            case 254: out.push_back(Reg::s(kM0)); break;
            default: break;
        }
    }
    return true;
}

using Effects = std::pair<std::vector<Reg>, std::vector<Reg>>;

std::optional<Effects> effects(const Instruction& inst, const Fields& fields, std::string_view name,
                               std::uint8_t lanes, std::uint16_t vgprs) {
    if (dropped(name)) return std::nullopt;
    const auto unsupported = [&]() { fail_unsupported_name(inst.offset, name); };
    const auto regs = [&](const Operand& op, std::vector<Reg>& out) {
        if (!add_regs(op, lanes, out)) detail::fail_isa(inst.offset, "register range out of bounds");
    };
    const auto mask = [&](std::uint8_t code, std::vector<Reg>& out) {
        for (std::uint8_t i = 0; i < lanes; ++i) out.push_back(Reg::s(static_cast<std::uint8_t>(code + i)));
    };
    const bool is_vector = !std::holds_alternative<SopFields>(fields) &&
                           !std::holds_alternative<SoppFields>(fields) &&
                           !std::holds_alternative<SmemFields>(fields);
    std::vector<Reg> r;
    std::vector<Reg> w;

    if (const auto* sop = std::get_if<SopFields>(&fields)) {
        if (name.starts_with("s_movrel") || name.starts_with("s_subvector_loop") ||
            name == "s_call_b64" || name == "s_rfe_b64")
            unsupported();
        for (const Operand& s : sop->src) regs(s, r);
        if (sop->dst) {
            regs(*sop->dst, w);
            if (name.starts_with("s_bitset") || name.starts_with("s_cmov") || name == "s_addk_i32" ||
                name == "s_mulk_i32")
                regs(*sop->dst, r);
        }
        if (name == "s_addc_u32" || name == "s_subb_u32" || name == "s_cmovk_i32" ||
            name.starts_with("s_cselect_") || name.starts_with("s_cmov_"))
            r.push_back(Reg::scc());
        if (name.find("saveexec") != std::string_view::npos || name.find("wrexec") != std::string_view::npos) {
            mask(kExec, r);
            mask(kExec, w);
        }
        if (writes_scc(inst.encoding, name)) w.push_back(Reg::scc());
    } else if (std::holds_alternative<SoppFields>(fields)) {
        if (name == "s_cbranch_scc0" || name == "s_cbranch_scc1") {
            r.push_back(Reg::scc());
        } else if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
            mask(kVcc, r);
        } else if (name == "s_cbranch_execz" || name == "s_cbranch_execnz") {
            mask(kExec, r);
        } else if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            r.push_back(Reg::s(kM0));
        } else if (name == "s_endpgm" || name == "s_endpgm_saved" || name == "s_endpgm_ordered_ps_done" ||
                   name == "s_code_end" || name == "s_branch" || name == "s_barrier" || name == "s_icache_inv" ||
                   name == "s_wakeup" || name == "s_round_mode" || name == "s_denorm_mode") {
        } else {
            unsupported();
        }
    } else if (const auto* smem = std::get_if<SmemFields>(&fields)) {
        if (name == "s_memtime" || name == "s_memrealtime" || name == "s_get_waveid_in_workgroup") {
            if (!smem->data) unsupported();
            regs(*smem->data, w);
        } else if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb") {
        } else if (name.find("_load_") != std::string_view::npos ||
                   name.find("_store_") != std::string_view::npos) {
            regs(smem->base, r);
            regs(smem->soffset, r);
            if (!smem->data) unsupported();
            regs(*smem->data, name.find("_load_") != std::string_view::npos ? w : r);
        } else {
            unsupported();
        }
    } else if (const auto* val = std::get_if<Valu>(&fields)) {
        bool src_indexed = false;
        bool dst_indexed = false;
        if (name == "v_movrels_b32") {
            src_indexed = true;
        } else if (name == "v_movreld_b32") {
            dst_indexed = true;
        } else if (name == "v_movrelsd_b32" || name == "v_movrelsd_2_b32" || name == "v_swaprel_b32") {
            src_indexed = true;
            dst_indexed = true;
        }
        const auto indexed = [&](const Operand& op, std::vector<Reg>& out) {
            const auto* vec = std::get_if<VectorOperand>(&op);
            if (vec == nullptr || vec->reg >= vgprs)
                detail::fail_isa(inst.offset, "relative VGPR base outside the program's VGPRs");
            for (std::uint16_t reg = vec->reg; reg < vgprs; ++reg)
                out.push_back(Reg::v(static_cast<std::uint8_t>(reg)));
        };
        for (std::size_t i = 0; i < val->src.size(); ++i) {
            if (i == 0 && src_indexed)
                indexed(val->src[i], r);
            else
                regs(val->src[i], r);
        }
        if (name.starts_with("v_cmpx")) {
            mask(kExec, w);
        } else if (val->dst) {
            if (dst_indexed)
                indexed(*val->dst, w);
            else
                regs(*val->dst, w);
        }
        if (val->carry) regs(*val->carry, w);
        if ((name == "v_dot4c_i32_i8" || name == "v_pk_fmac_f16") && val->dst) regs(*val->dst, r);
        if (name == "v_swap_b32") {
            regs(val->src[0], w);
            if (val->dst) regs(*val->dst, r);
        } else if (name == "v_swaprel_b32") {
            indexed(val->src[0], w);
            if (val->dst) indexed(*val->dst, r);
        }
        if (name.starts_with("v_interp_") || src_indexed || dst_indexed) r.push_back(Reg::s(kM0));
    } else if (const auto* intrp = std::get_if<VintrpFields>(&fields)) {
        regs(intrp->src, r);
        r.push_back(Reg::s(kM0));
        regs(intrp->dst, w);
    } else if (const auto* ds = std::get_if<DsFields>(&fields)) {
        if (ds->addr) regs(*ds->addr, r);
        for (const Operand& d : ds->data) regs(d, r);
        if (ds->dst) regs(*ds->dst, w);
        if (ds->gds || name.starts_with("ds_gws") || name.find("addtid") != std::string_view::npos ||
            name == "ds_ordered_count" || name == "ds_append" || name == "ds_consume")
            r.push_back(Reg::s(kM0));
    } else if (const auto* buffer = std::get_if<BufferFields>(&fields)) {
        if (buffer->offen || buffer->idxen) regs(buffer->addr, r);
        regs(buffer->rsrc, r);
        regs(buffer->soffset, r);
        if (buffer->lds) r.push_back(Reg::s(kM0));
        if (buffer->data) {
            if (name.find("_load_") != std::string_view::npos) {
                if (!buffer->lds) regs(*buffer->data, w);
            } else if (name.find("_store_") != std::string_view::npos) {
                regs(*buffer->data, r);
            } else if (name.find("_atomic_") != std::string_view::npos) {
                regs(*buffer->data, r);
                if (buffer->cache.glc) regs(atomic_result(*buffer->data, name), w);
            } else {
                unsupported();
            }
        }
    } else if (const auto* image = std::get_if<ImageFields>(&fields)) {
        for (const Operand& a : image->addr) regs(a, r);
        regs(image->rsrc, r);
        if (image->sampler) regs(*image->sampler, r);
        if (name.find("_store") != std::string_view::npos) {
            regs(image->data, r);
        } else if (name.find("_atomic_") != std::string_view::npos) {
            regs(image->data, r);
            if (image->cache.glc) regs(atomic_result(image->data, name), w);
        } else {
            regs(image->data, w);
        }
    } else if (const auto* flat = std::get_if<FlatFields>(&fields)) {
        if (flat->addr) regs(*flat->addr, r);
        if (flat->data) regs(*flat->data, r);
        if (flat->saddr) regs(*flat->saddr, r);
        if (flat->dst) regs(*flat->dst, w);
    } else if (const auto* exp = std::get_if<ExpFields>(&fields)) {
        for (const auto& s : exp->src)
            if (s) regs(*s, r);
    }

    if (is_vector) {
        mask(kExec, r);
        for (Reg reg : w)
            if (reg.kind == Reg::Kind::V) r.push_back(reg);
    }
    std::vector<Reg> seen;
    std::vector<Reg> unique;
    unique.reserve(r.size());
    for (Reg reg : r) {
        if (std::find(seen.begin(), seen.end(), reg) == seen.end()) {
            seen.push_back(reg);
            unique.push_back(reg);
        }
    }
    return Effects{std::move(unique), std::move(w)};
}

void remove_trivial_phis(std::vector<Block>& blocks, std::uint32_t values) {
    std::vector<Value> replaced;
    replaced.reserve(values);
    for (std::uint32_t i = 0; i < values; ++i) replaced.push_back(Value{i});
    const auto find = [&replaced](Value value) {
        while (replaced[value.index] != value) value = replaced[value.index];
        return value;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (Block& block : blocks) {
            std::vector<Phi> kept;
            kept.reserve(block.phis.size());
            for (const Phi& phi : block.phis) {
                std::optional<Value> only;
                bool remove = false;
                for (Value arg : phi.args) {
                    const Value a = find(arg);
                    if (a == phi.value || (only && a == *only)) continue;
                    if (only) {
                        remove = true;
                        break;
                    }
                    only = a;
                }
                if (remove) {
                    kept.push_back(phi);
                    continue;
                }
                if (!only) {
                    kept.push_back(phi);
                    continue;
                }
                replaced[phi.value.index] = *only;
                changed = true;
            }
            block.phis = std::move(kept);
        }
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis)
            for (Value& arg : phi.args) arg = find(arg);
        for (Inst& inst : block.insts)
            for (auto& entry : inst.reads) entry.second = find(entry.second);
    }
}

}  // namespace

std::vector<std::size_t> Exit::successors() const {
    switch (kind) {
        case Kind::Jump: return {target};
        case Kind::Branch: return {target, not_taken};
        case Kind::End:
        case Kind::Indirect: return {};
    }
    return {};
}

std::string_view Inst::name() const { return machine.name().value_or(std::string_view{}); }

std::optional<Value> Inst::read(Reg reg) const {
    for (const auto& [r, v] : reads)
        if (r == reg) return v;
    return std::nullopt;
}

std::optional<Value> Inst::write(Reg reg) const {
    for (const auto& [r, v] : writes)
        if (r == reg) return v;
    return std::nullopt;
}

std::vector<std::size_t> reverse_postorder(const std::vector<Block>& blocks) {
    std::vector<std::size_t> post;
    if (blocks.empty()) return post;
    std::vector<bool> seen(blocks.size(), false);
    post.reserve(blocks.size());
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        const std::size_t b = stack.back().first;
        const std::size_t at = stack.back().second++;
        const std::vector<std::size_t> successors = blocks[b].exit.successors();
        if (at < successors.size()) {
            const std::size_t s = successors[at];
            if (!seen[s]) {
                seen[s] = true;
                stack.emplace_back(s, 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::reverse(post.begin(), post.end());
    return post;
}

std::vector<std::size_t> idoms(const std::vector<Block>& blocks, const std::vector<std::size_t>& order,
                               const std::vector<std::size_t>& index) {
    std::vector<std::size_t> idom(blocks.size(), kNoBlock);
    if (order.empty()) return idom;
    const std::size_t entry = order.front();
    idom[entry] = entry;
    const auto intersect = [&](std::size_t a, std::size_t b) {
        while (a != b) {
            while (index[a] > index[b]) a = idom[a];
            while (index[b] > index[a]) b = idom[b];
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t k = 1; k < order.size(); ++k) {
            const std::size_t b = order[k];
            std::size_t next = kNoBlock;
            for (std::size_t q : blocks[b].preds) {
                if (idom[q] == kNoBlock) continue;
                next = next == kNoBlock ? q : intersect(q, next);
            }
            if (idom[b] != next) {
                idom[b] = next;
                changed = true;
            }
        }
    }
    return idom;
}

std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(const std::vector<Block>& blocks,
                                                                   const std::vector<std::size_t>& order,
                                                                   const std::vector<std::size_t>& index) {
    if (order.empty()) return std::nullopt;
    const std::size_t entry = order.front();
    const std::vector<std::size_t> idom = idoms(blocks, order, index);
    for (std::size_t b : order) {
        for (std::size_t s : blocks[b].exit.successors()) {
            if (index[s] > index[b]) continue;
            std::size_t d = b;
            while (d != s && d != entry) d = idom[d];
            if (d != s) return std::pair<std::size_t, std::size_t>{b, s};
        }
    }
    return std::nullopt;
}

Program build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs) {
    const std::vector<Instruction> list = decode(code);
    const std::uint8_t lanes = static_cast<std::uint8_t>(1 + (wave64 ? 1 : 0));

    std::vector<Control> flows;
    flows.reserve(list.size());
    std::vector<std::size_t> leaders{0};
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::span<const Instruction> preceding(list.data(), i);
        const Control flow = control(list[i], code.size(), preceding);
        if (flow.kind == Control::Kind::Jump || flow.kind == Control::Kind::Branch) leaders.push_back(flow.target);
        if (flow.kind != Control::Kind::Next && i + 1 < list.size()) leaders.push_back(list[i + 1].offset);
        flows.push_back(flow);
    }
    std::sort(leaders.begin(), leaders.end());
    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
    const auto block_at = [&](std::size_t offset) {
        const auto it = std::lower_bound(leaders.begin(), leaders.end(), offset);
        if (it == leaders.end() || *it != offset) detail::fail_isa(offset, "jump into a block");
        return static_cast<std::size_t>(it - leaders.begin());
    };

    std::vector<Block> blocks;
    blocks.reserve(leaders.size());
    for (std::size_t offset : leaders) blocks.push_back(Block{offset, {}, {}, {}, Exit::end()});

    std::vector<std::optional<std::size_t>> next_offsets;
    next_offsets.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
        next_offsets.push_back(i + 1 < list.size() ? std::optional<std::size_t>{list[i + 1].offset} : std::nullopt);

    for (std::size_t i = 0; i < list.size(); ++i) {
        const Instruction& machine = list[i];
        const Control& flow = flows[i];
        const std::optional<std::size_t>& next = next_offsets[i];
        const std::size_t b =
            static_cast<std::size_t>(std::upper_bound(leaders.begin(), leaders.end(), machine.offset) -
                                     leaders.begin()) -
            1;
        if (!machine.name()) detail::fail_unsupported(machine.offset, "unknown opcode");
        const std::string_view name = *machine.name();
        const Fields fields = machine.fields(wave64);
        const auto fallthrough = [&]() {
            if (!next) detail::fail_isa(machine.offset, "falls off the code");
            return block_at(*next);
        };
        const bool ends =
            flow.kind != Control::Kind::Next || !next || std::binary_search(leaders.begin(), leaders.end(), *next);
        if (ends) {
            switch (flow.kind) {
                case Control::Kind::Next: blocks[b].exit = Exit::jump(fallthrough()); break;
                case Control::Kind::Jump: blocks[b].exit = Exit::jump(block_at(flow.target)); break;
                case Control::Kind::Branch:
                    blocks[b].exit = Exit::branch(block_at(flow.target), fallthrough());
                    break;
                case Control::Kind::Stop:
                    blocks[b].exit = name == "s_setpc_b64" ? Exit::indirect() : Exit::end();
                    break;
            }
        }
        if (auto found = effects(machine, fields, name, lanes, vgprs)) {
            Inst inst;
            inst.machine = machine;
            inst.fields = fields;
            for (Reg reg : found->first) inst.reads.emplace_back(reg, Value{kNoValue});
            for (Reg reg : found->second) inst.writes.emplace_back(reg, Value{kNoValue});
            blocks[b].insts.push_back(std::move(inst));
        }
    }
    link(blocks);
    if (!blocks[0].preds.empty()) detail::fail_unsupported(0, "a branch to the program entry");
    split_irreducible(blocks);

    const std::vector<std::size_t> order = reverse_postorder(blocks);
    auto [live_in, live_out] = liveness(blocks, order);
    std::uint32_t values = 0;
    const auto new_value = [&values]() { return Value{values++}; };
    std::vector<std::pair<Reg, Value>> inputs;
    std::vector<std::vector<std::pair<Reg, Value>>> outs(blocks.size());
    std::array<Value, kRegs> current{};
    for (std::size_t b : order) {
        current.fill(Value{kNoValue});
        Block& block = blocks[b];
        for (Reg reg : members(live_in[b])) {
            Value value;
            if (block.preds.empty()) {
                value = new_value();
                inputs.emplace_back(reg, value);
            } else if (block.preds.size() == 1) {
                value = lookup(outs[block.preds[0]], reg);
            } else {
                value = new_value();
                block.phis.push_back(Phi{reg, value, {}});
            }
            current[reg_index(reg)] = value;
        }
        for (Inst& inst : block.insts) {
            for (auto& [reg, value] : inst.reads) value = current[reg_index(reg)];
            for (auto& [reg, value] : inst.writes) {
                value = new_value();
                current[reg_index(reg)] = value;
            }
        }
        for (Reg reg : members(live_out[b])) outs[b].emplace_back(reg, current[reg_index(reg)]);
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis)
            for (std::size_t p : block.preds) phi.args.push_back(lookup(outs[p], phi.reg));
    }
    remove_trivial_phis(blocks, values);
    Program program;
    program.wave64 = wave64;
    program.vgprs = vgprs;
    program.blocks = std::move(blocks);
    program.inputs = std::move(inputs);
    return program;
}

}  // namespace g6shim::shader
