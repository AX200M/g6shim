// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/wave.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/resource.hpp"

namespace g6shim::shader::emit {

namespace {

constexpr std::uint32_t kWaveSize32 = 32;
constexpr std::uint32_t kWaveSize64 = 64;
constexpr std::uint8_t kExecAllOr = 193;
constexpr std::uint8_t kExecAllNand = 128;

constexpr std::uint32_t kIdentityAddFloatSign = 0x8000'0000;
constexpr std::uint32_t kIdentityMinInt = 0x7FFF'FFFF;
constexpr std::uint32_t kIdentityMaxInt = 0x8000'0000;
constexpr std::uint32_t kIdentityMinFloat = 0x7F80'0000;
constexpr std::uint32_t kIdentityMaxFloat = 0xFF80'0000;

std::vector<Sym> lane_values(std::uint32_t wave, const Sym& value) {
    return std::vector<Sym>(wave, value);
}

std::uint32_t reverse_bits(std::uint32_t value) {
    value = ((value & 0x5555'5555u) << 1) | ((value >> 1) & 0x5555'5555u);
    value = ((value & 0x3333'3333u) << 2) | ((value >> 2) & 0x3333'3333u);
    value = ((value & 0x0F0F'0F0Fu) << 4) | ((value >> 4) & 0x0F0F'0F0Fu);
    value = ((value & 0x00FF'00FFu) << 8) | ((value >> 8) & 0x00FF'00FFu);
    return (value << 16) | (value >> 16);
}

Sym combine(Op op, const Sym& a, const Sym& b) {
    if (a.kind == Sym::Kind::K && identity_of(op, a.constant)) return b;
    if (b.kind == Sym::Kind::K && identity_of(op, b.constant)) return a;
    if (a.kind == Sym::Kind::Red && b.kind == Sym::Kind::Red && a.src == b.src && a.mask == b.mask &&
        a.ident == b.ident && identity_of(op, a.ident) && (!a.has_op || a.op == op) && (!b.has_op || b.op == op) &&
        (!counts_lanes(op) || (a.lanes & b.lanes) == 0)) {
        return Sym::red(op, a.src, a.mask, a.ident, a.lanes | b.lanes);
    }
    return Sym::unknown();
}

std::optional<std::uint64_t> constant_of(const Inst& inst) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr || fields->src.empty()) return std::nullopt;
    const Operand& first = fields->src.front();
    if (const auto* constant = std::get_if<ConstantOperand>(&first)) {
        const std::optional<std::uint32_t> value = inline_constant(constant->code);
        if (!value) return std::nullopt;
        if (static_cast<std::int32_t>(*value) < 0) return UINT64_MAX;
        return *value;
    }
    if (const auto* literal = std::get_if<LiteralOperand>(&first)) return literal->value;
    return std::nullopt;
}

std::optional<Shape> single_shape(const Sym& value, std::uint64_t all) {
    switch (value.kind) {
        case Sym::Kind::K: return Shape::k(value.constant);
        case Sym::Kind::Own:
        case Sym::Kind::Uni: return Shape::of_value(value.value);
        case Sym::Kind::Red:
            if (value.has_op && value.lanes == all)
                return Shape::active(value.op, value.src, value.mask, value.ident);
            return std::nullopt;
        case Sym::Kind::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

std::unordered_map<Value, Shape, ValueHash> collect_shapes(
    const std::unordered_map<Value, std::vector<Sym>, ValueHash>& vec,
    const std::unordered_map<Value, Sym, ValueHash>& uni, std::uint32_t wave) {
    const std::uint64_t all = wave == kWaveSize64 ? UINT64_MAX : static_cast<std::uint64_t>(UINT32_MAX);
    std::unordered_map<Value, Shape, ValueHash> out;
    for (const auto& [value, symbol] : uni) {
        const std::optional<Shape> shape = single_shape(symbol, all);
        if (shape) out.emplace(value, *shape);
    }
    for (const auto& [value, lanes] : vec) {
        const std::optional<Shape> shape = per_lane(lanes, all);
        if (shape) out.emplace(value, *shape);
    }
    return out;
}

std::unordered_map<Value, Shape, ValueHash> simulate(std::span<const Inst> insts, std::uint32_t wave,
                                                     Value saved, std::size_t restore) {
    const std::uint64_t all =
        wave == kWaveSize64 ? UINT64_MAX : static_cast<std::uint64_t>(UINT32_MAX);
    std::uint64_t exec = all;
    std::unordered_map<Value, std::vector<Sym>, ValueHash> vec;
    std::unordered_map<Value, Sym, ValueHash> uni;
    std::vector<Value> masked{saved};
    for (std::size_t index = 0; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const std::string_view name = inst.name();
        if (index == restore) continue;
        if (inst.write(Reg::s(kExec))) {
            std::optional<std::uint64_t> low;
            if (name.starts_with("s_mov_b")) low = constant_of(inst);
            if (!low) return collect_shapes(vec, uni, wave);
            exec = name == "s_mov_b64" ? *low
                                       : (exec & ~std::uint64_t{0xFFFF'FFFFu}) | (*low & 0xFFFF'FFFFu);
            continue;
        }
        if (inst.write(Reg::s(static_cast<std::uint8_t>(kExec + 1)))) {
            std::optional<std::uint64_t> high;
            if (name.starts_with("s_mov_b")) high = constant_of(inst);
            if (!high) return collect_shapes(vec, uni, wave);
            exec = (exec & 0xFFFF'FFFFu) | (*high << 32);
            continue;
        }
        if (const auto* fields = std::get_if<SopFields>(&inst.fields)) {
            if (name.starts_with("s_and_b")) {
                const bool from_mask = std::any_of(fields->src.begin(), fields->src.end(), [&](const Operand& op) {
                    const auto* scalar = std::get_if<ScalarOperand>(&op);
                    if (scalar == nullptr) return false;
                    const auto value = inst.read(Reg::s(scalar->code));
                    return value && std::find(masked.begin(), masked.end(), *value) != masked.end();
                });
                if (from_mask) {
                    for (const auto& written : inst.writes) {
                        if (written.first.kind == Reg::Kind::S) {
                            masked.push_back(written.second);
                            break;
                        }
                    }
                }
            }
            for (const auto& written : inst.writes) uni.try_emplace(written.second, Sym::unknown());
            continue;
        }
        const auto* valu = std::get_if<Valu>(&inst.fields);
        if (valu == nullptr) {
            for (const auto& written : inst.writes) vec[written.second] = lane_values(wave, Sym::unknown());
            continue;
        }
        const auto lanes = [&](const Operand& operand) -> std::vector<Sym> {
            if (const auto* vector = std::get_if<VectorOperand>(&operand)) {
                const auto read = inst.read(Reg::v(vector->reg));
                if (!read) return lane_values(wave, Sym::unknown());
                const auto found = vec.find(*read);
                if (found != vec.end()) return found->second;
                return lane_values(wave, Sym::own(*read));
            }
            if (const auto* scalar = std::get_if<ScalarOperand>(&operand)) {
                if (scalar->code == kNull) return lane_values(wave, Sym::unknown());
                const auto read = inst.read(Reg::s(scalar->code));
                if (!read) return lane_values(wave, Sym::unknown());
                const auto found = uni.find(*read);
                return lane_values(wave, found != uni.end() ? found->second : Sym::uni(*read));
            }
            if (const auto* constant = std::get_if<ConstantOperand>(&operand)) {
                const std::optional<std::uint32_t> value = inline_constant(constant->code);
                return lane_values(wave, value ? Sym::k(*value) : Sym::unknown());
            }
            if (const auto* literal = std::get_if<LiteralOperand>(&operand))
                return lane_values(wave, Sym::k(literal->value));
            return lane_values(wave, Sym::unknown());
        };
        const bool modifiers =
            valu->abs != 0 || valu->neg != 0 || valu->clamp || valu->omod != 0 || valu->sdwa.has_value();
        if (name == "v_readlane_b32") {
            const auto* destination =
                valu->dst ? std::get_if<ScalarOperand>(&*valu->dst) : nullptr;
            if (destination == nullptr || valu->src.size() < 2) continue;
            std::optional<std::uint32_t> lane;
            if (const auto* constant = std::get_if<ConstantOperand>(&valu->src[1]))
                lane = inline_constant(constant->code);
            else if (const auto* literal = std::get_if<LiteralOperand>(&valu->src[1]))
                lane = literal->value;
            const std::vector<Sym> values = lanes(valu->src[0]);
            if (const auto written = inst.write(Reg::s(destination->code))) {
                Sym value = Sym::unknown();
                if (lane && *lane < values.size()) value = values[*lane];
                uni[*written] = value;
            }
            continue;
        }
        const auto* destination = valu->dst ? std::get_if<VectorOperand>(&*valu->dst) : nullptr;
        if (destination == nullptr) {
            for (const auto& written : inst.writes) uni[written.second] = Sym::unknown();
            continue;
        }
        const auto written = inst.write(Reg::v(destination->reg));
        if (!written) continue;
        const std::vector<Sym> old = lanes(VectorOperand{destination->reg, 1});
        std::optional<std::vector<Sym>> src0;
        if (!valu->src.empty()) src0 = lanes(valu->src[0]);
        std::uint64_t writes = exec;
        std::optional<std::vector<Sym>> fetched;
        if (src0) {
            if (valu->dpp) {
                if (exec != all) {
                    vec[*written] = lane_values(wave, Sym::unknown());
                    continue;
                }
                std::vector<Sym> values;
                values.reserve(wave);
                bool ok = true;
                for (std::uint32_t lane = 0; lane < wave; ++lane) {
                    const auto at = dpp_lane(*valu->dpp, lane);
                    if (!at) {
                        ok = false;
                        continue;
                    }
                    const auto [source, zero, on] = *at;
                    if (source >= wave) {
                        ok = false;
                        continue;
                    }
                    values.push_back(zero ? Sym::k(0) : (*src0)[source]);
                    if (!on) writes &= ~(std::uint64_t{1} << lane);
                }
                if (ok) fetched = std::move(values);
            } else {
                fetched = *src0;
            }
        }
        const auto source = [&](std::size_t k) -> std::optional<std::vector<Sym>> {
            if (k >= valu->src.size()) return std::nullopt;
            return lanes(valu->src[k]);
        };
        std::vector<Sym> result;
        if (modifiers) {
            result = lane_values(wave, Sym::unknown());
        } else if (name == "v_mov_b32" && fetched) {
            result = *fetched;
        } else if (name == "v_bfrev_b32" && fetched) {
            result.reserve(wave);
            for (const Sym& value : *fetched)
                result.push_back(value.kind == Sym::Kind::K ? Sym::k(reverse_bits(value.constant))
                                                           : Sym::unknown());
        } else if (name == "v_cndmask_b32" && fetched) {
            std::optional<Value> mask;
            if (valu->src.size() > 2)
                if (const auto* scalar = std::get_if<ScalarOperand>(&valu->src[2]))
                    mask = inst.read(Reg::s(scalar->code));
            const std::vector<Sym> other = source(1).value_or(std::vector<Sym>{});
            result.reserve(wave);
            for (std::uint32_t lane = 0; lane < wave; ++lane) {
                const Sym& value = (*fetched)[lane];
                if (value.kind == Sym::Kind::K && lane < other.size() && other[lane].kind == Sym::Kind::Own &&
                    mask && std::find(masked.begin(), masked.end(), *mask) != masked.end()) {
                    result.push_back(Sym::red(std::nullopt, other[lane].value, *mask, value.constant,
                                              std::uint64_t{1} << lane));
                } else {
                    result.push_back(Sym::unknown());
                }
            }
        } else if (name == "v_permlanex16_b32" && fetched) {
            const auto select = [&](std::size_t k) -> std::optional<std::uint32_t> {
                if (k >= valu->src.size()) return std::nullopt;
                if (const auto* constant = std::get_if<ConstantOperand>(&valu->src[k]))
                    return inline_constant(constant->code);
                if (const auto* literal = std::get_if<LiteralOperand>(&valu->src[k]))
                    return literal->value;
                return std::nullopt;
            };
            const std::optional<std::uint32_t> first = select(1);
            const std::optional<std::uint32_t> second = select(2);
            if (first && second) {
                const std::uint64_t selects =
                    static_cast<std::uint64_t>(*first) | static_cast<std::uint64_t>(*second) << 32;
                for (std::uint32_t lane = 0; lane < wave; ++lane) {
                    const std::uint32_t other = (lane & ~31u) | ((lane & 16u) ^ 16u);
                    const std::uint32_t pick =
                        static_cast<std::uint32_t>((selects >> (4 * (lane & 15))) & 15u);
                    result.push_back((*fetched)[other | pick]);
                }
            } else {
                result = lane_values(wave, Sym::unknown());
            }
        } else if (name == "v_sub_nc_u32" && fetched) {
            const std::vector<Sym> subtract = source(1).value_or(std::vector<Sym>{});
            result.reserve(wave);
            for (std::uint32_t lane = 0; lane < wave; ++lane) {
                const Sym& value = (*fetched)[lane];
                Sym combined = Sym::unknown();
                if (value.kind == Sym::Kind::Red && (!value.has_op || value.op == Op::AddU) &&
                    value.ident == 0 && lane < subtract.size()) {
                    const Sym& other = subtract[lane];
                    if (other.kind == Sym::Kind::Red && (!other.has_op || other.op == Op::AddU) &&
                        other.ident == 0 && value.src == other.src && value.mask == other.mask &&
                        (value.lanes & other.lanes) == other.lanes) {
                        const std::uint64_t rest = value.lanes & ~other.lanes;
                        combined = rest == 0 ? Sym::k(0)
                                             : Sym::red(Op::AddU, value.src, value.mask, 0, rest);
                    }
                }
                result.push_back(combined);
            }
        } else if (fetched) {
            const std::optional<Op> op = op_of(name);
            const auto other = source(1);
            if (op && other) {
                result.reserve(wave);
                for (std::uint32_t lane = 0; lane < wave; ++lane)
                    result.push_back(combine(*op, (*fetched)[lane], (*other)[lane]));
            } else {
                result = lane_values(wave, Sym::unknown());
            }
        } else {
            result = lane_values(wave, Sym::unknown());
        }
        std::vector<Sym> merged;
        merged.reserve(wave);
        for (std::uint32_t lane = 0; lane < wave; ++lane)
            merged.push_back((writes >> lane & 1) != 0 ? result[lane] : old[lane]);
        vec[*written] = std::move(merged);
    }
    return collect_shapes(vec, uni, wave);
}

std::uint8_t saved_code(const Inst& inst) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr || !fields->dst) return kNull;
    const auto* scalar = std::get_if<ScalarOperand>(&*fields->dst);
    return scalar == nullptr ? kNull : scalar->code;
}

}  // namespace

std::optional<Op> op_of(std::string_view name) {
    if (name == "v_add_nc_u32") return Op::AddU;
    if (name == "v_add_f32") return Op::AddF;
    if (name == "v_min_u32") return Op::MinU;
    if (name == "v_max_u32") return Op::MaxU;
    if (name == "v_min_i32") return Op::MinI;
    if (name == "v_max_i32") return Op::MaxI;
    if (name == "v_min_f32") return Op::MinF;
    if (name == "v_max_f32") return Op::MaxF;
    if (name == "v_or_b32") return Op::Or;
    if (name == "v_and_b32") return Op::And;
    if (name == "v_xor_b32") return Op::Xor;
    return std::nullopt;
}

bool identity_of(Op op, std::uint32_t constant) {
    switch (op) {
        case Op::AddU:
        case Op::Or:
        case Op::Xor:
        case Op::MaxU: return constant == 0;
        case Op::AddF: return constant == 0 || constant == kIdentityAddFloatSign;
        case Op::MinU:
        case Op::And: return constant == UINT32_MAX;
        case Op::MinI: return constant == kIdentityMinInt;
        case Op::MaxI: return constant == kIdentityMaxInt;
        case Op::MinF: return constant == kIdentityMinFloat;
        case Op::MaxF: return constant == kIdentityMaxFloat;
    }
    return false;
}

bool counts_lanes(Op op) { return op == Op::AddU || op == Op::AddF || op == Op::Xor; }

bool reads_lane_of_section(const std::vector<Inst>& insts, std::size_t start, std::size_t end, const Inst& inst) {
    if (inst.name() != "v_readlane_b32") return false;
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr || valu->src.empty()) return false;
    const auto* vector = std::get_if<VectorOperand>(&valu->src.front());
    if (vector == nullptr) return false;
    const auto read = inst.read(Reg::v(vector->reg));
    if (!read) return false;
    for (std::size_t at = start + 1; at < end; ++at)
        for (const auto& written : insts[at].writes)
            if (written.second == *read) return true;
    return false;
}

std::unordered_map<std::size_t, Section> sections(const Program& program, std::size_t block) {
    const std::vector<Inst>& insts = program.blocks[block].insts;
    const std::uint32_t wave = program.wave64 ? kWaveSize64 : kWaveSize32;
    std::unordered_map<std::size_t, Section> out;
    std::size_t i = 0;
    while (i < insts.size()) {
        const Inst& inst = insts[i];
        const std::string_view name = inst.name();
        std::optional<std::uint8_t> all;
        if (name == "s_or_saveexec_b64" || name == "s_or_saveexec_b32")
            all = kExecAllOr;
        else if (name == "s_nand_saveexec_b64" || name == "s_nand_saveexec_b32")
            all = kExecAllNand;
        bool start = false;
        if (all)
            if (const auto* fields = std::get_if<SopFields>(&inst.fields); fields && !fields->src.empty())
                if (const auto* constant = std::get_if<ConstantOperand>(&fields->src.front()))
                    start = constant->code == *all;
        std::optional<Value> saved;
        if (start) {
            const auto* fields = std::get_if<SopFields>(&inst.fields);
            if (fields && fields->dst)
                if (const auto* scalar = std::get_if<ScalarOperand>(&*fields->dst))
                    saved = inst.write(Reg::s(scalar->code));
        }
        if (!saved) {
            ++i;
            continue;
        }
        const std::uint8_t restore = saved_code(inst);
        std::optional<std::size_t> end;
        for (std::size_t j = i + 1; j < insts.size(); ++j) {
            if (!insts[j].write(Reg::s(kExec))) continue;
            const auto read = insts[j].read(Reg::s(restore));
            if (read && *read == *saved) {
                end = j;
                break;
            }
        }
        if (!end) break;
        std::size_t last = *end;
        while (last + 1 < insts.size() && reads_lane_of_section(insts, i, *end, insts[last + 1])) ++last;
        const std::size_t stop = last == *end ? *end : last + 1;
        std::unordered_map<Value, Shape, ValueHash> shapes =
            simulate(std::span<const Inst>(insts).subspan(i + 1, stop - (i + 1)), wave, *saved, *end - (i + 1));
        out.emplace(i, Section{stop, std::move(shapes)});
        i = stop;
    }
    return out;
}

std::optional<std::tuple<std::uint32_t, bool, bool>> dpp_lane(const Dpp& dpp, std::uint32_t lane) {
    if (const auto* lanes = std::get_if<DppLanes>(&dpp))
        return std::tuple<std::uint32_t, bool, bool>{
            (lane & ~7u) | ((lanes->sel >> (3 * (lane & 7))) & 7u), false, true};
    const DppRow& row = std::get<DppRow>(dpp);
    const std::uint32_t row_base = lane & ~15u;
    const std::uint32_t position = lane & 15u;
    const std::uint32_t n = row.ctrl & 15u;
    const bool enabled = ((row.row_mask >> (lane >> 4 & 3)) & 1) != 0 &&
                         ((row.bank_mask >> (lane >> 2 & 3)) & 1) != 0;
    std::uint32_t source = 0;
    bool inside = false;
    if (row.ctrl <= 0xFF) {
        source = (lane & ~3u) | ((static_cast<std::uint32_t>(row.ctrl) >> (2 * (lane & 3))) & 3u);
        inside = true;
    } else if (row.ctrl >= 0x101 && row.ctrl <= 0x10F) {
        source = lane + n;
        inside = position + n < 16;
    } else if (row.ctrl >= 0x111 && row.ctrl <= 0x11F) {
        source = lane - n;
        inside = position >= n;
    } else if (row.ctrl >= 0x121 && row.ctrl <= 0x12F) {
        source = row_base | ((position - n) & 15u);
        inside = true;
    } else if (row.ctrl == 0x140) {
        source = lane ^ 15u;
        inside = true;
    } else if (row.ctrl == 0x141) {
        source = lane ^ 7u;
        inside = true;
    } else if (row.ctrl >= 0x150 && row.ctrl <= 0x15F) {
        source = row_base | n;
        inside = true;
    } else if (row.ctrl >= 0x160 && row.ctrl <= 0x16F) {
        source = lane ^ n;
        inside = true;
    } else {
        return std::nullopt;
    }
    if (inside) return std::tuple<std::uint32_t, bool, bool>{source, false, enabled};
    if (row.bound_ctrl) return std::tuple<std::uint32_t, bool, bool>{lane, true, enabled};
    return std::tuple<std::uint32_t, bool, bool>{lane, false, false};
}

std::optional<Shape> per_lane(std::span<const Sym> lanes, std::uint64_t all) {
    if (lanes.empty()) return std::nullopt;
    const Sym first = lanes[0];
    const auto own = [](std::size_t lane, const Sym& value) -> std::optional<std::tuple<Value, Value, std::uint32_t>> {
        if (value.kind == Sym::Kind::Red && !value.has_op && value.lanes == (std::uint64_t{1} << lane))
            return std::tuple<Value, Value, std::uint32_t>{value.src, value.mask, value.ident};
        return std::nullopt;
    };
    if (const auto masked = own(0, first)) {
        bool same = true;
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            if (own(lane, lanes[lane]) != masked) {
                same = false;
                break;
            }
        }
        if (same) return Shape::masked(std::get<0>(*masked), std::get<1>(*masked), std::get<2>(*masked));
    }
    if (std::all_of(lanes.begin(), lanes.end(), [&first](const Sym& value) { return value == first; })) {
        switch (first.kind) {
            case Sym::Kind::K: return Shape::k(first.constant);
            case Sym::Kind::Own:
            case Sym::Kind::Uni: return Shape::of_value(first.value);
            case Sym::Kind::Red:
                if (first.has_op && first.lanes == all)
                    return Shape::active(first.op, first.src, first.mask, first.ident);
                return std::nullopt;
            case Sym::Kind::Unknown: return std::nullopt;
        }
    }
    using Prefix = std::tuple<Op, Value, Value, std::uint32_t, bool>;
    std::optional<Prefix> found;
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        const std::uint64_t below = (std::uint64_t{1} << lane) - 1;
        const Sym& value = lanes[lane];
        if (value.kind == Sym::Kind::K && lane == 0) continue;
        if (value.kind != Sym::Kind::Red) return std::nullopt;
        const bool inclusive = value.lanes == (below | std::uint64_t{1} << lane);
        if (!inclusive && value.lanes != below) return std::nullopt;
        Op op = Op::AddU;
        if (value.has_op) {
            op = value.op;
        } else if (found) {
            op = std::get<0>(*found);
        } else {
            continue;
        }
        const Prefix me{op, value.src, value.mask, value.ident, inclusive};
        if (!found) {
            found = me;
        } else if (!(*found == me)) {
            return std::nullopt;
        }
    }
    if (!found) return std::nullopt;
    const auto [op, src, mask, ident, inclusive] = *found;
    if (lanes[0].kind == Sym::Kind::K && (inclusive || lanes[0].constant != ident)) return std::nullopt;
    return Shape::prefix(op, src, mask, ident, inclusive);
}

}  // namespace g6shim::shader::emit
