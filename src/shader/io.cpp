// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"

namespace g6shim::shader {

namespace {

struct IoError {
    const char* what;
};

struct BarycentricInput {
    std::uint8_t count = 0;
    std::optional<Barycentric> kind;
};

constexpr std::array<BarycentricInput, 16> kPsVgprs{{
    {2, Barycentric::PerspectiveSample},
    {2, Barycentric::PerspectiveCenter},
    {2, Barycentric::PerspectiveCentroid},
    {3, Barycentric::PerspectivePullModel},
    {2, Barycentric::LinearSample},
    {2, Barycentric::LinearCenter},
    {2, Barycentric::LinearCentroid},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
}};

Input ps_system(std::size_t bit) {
    if (bit == 7) return Input::simple(Input::Kind::LineStipple);
    if (bit >= 8 && bit <= 11) return Input::of_position(static_cast<std::uint8_t>(bit - 8));
    if (bit == 12) return Input::simple(Input::Kind::FrontFace);
    if (bit == 13) return Input::simple(Input::Kind::Ancillary);
    if (bit == 14) return Input::simple(Input::Kind::SampleCoverage);
    return Input::simple(Input::Kind::FixedPosition);
}

std::vector<std::pair<std::uint8_t, Input>> ps_vgprs(std::uint32_t addr) {
    std::vector<std::pair<std::uint8_t, Input>> out;
    std::uint8_t next = 0;
    for (std::size_t bit = 0; bit < kPsVgprs.size(); ++bit) {
        if (((addr >> bit) & 1) == 0) continue;
        const BarycentricInput& entry = kPsVgprs[bit];
        for (std::uint8_t index = 0; index < entry.count; ++index) {
            Input input = entry.kind ? Input::of_barycentric(*entry.kind, index) : ps_system(bit);
            out.emplace_back(static_cast<std::uint8_t>(next + index), input);
        }
        next = static_cast<std::uint8_t>(next + entry.count);
    }
    return out;
}

struct Pair {
    Barycentric bary = Barycentric::PerspectiveSample;
    std::vector<Site> first;

    Interp with(const Attribute& attribute) const { return Interp::interpolated(attribute, bary, first); }
};

class Halves {
public:
    Halves(const Program& program, const std::unordered_map<Value, Site, ValueHash>& defs,
           const std::unordered_map<Value, Site, ValueHash>& phis)
        : p_(&program), defs_(&defs), phis_(&phis) {}

    Pair pair(const Operand& attr, std::optional<Value> acc, std::optional<Value> j,
              const std::function<std::optional<std::pair<Barycentric, std::uint8_t>>(std::optional<Value>)>&
                  barycentric) const {
        if (!acc) throw IoError{"an interpolation without its first half"};
        std::vector<Value> seen;
        std::vector<Site> first;
        sites(*acc, seen, first);
        const auto known = barycentric(j);
        if (!known || known->second != 1) throw IoError{"an interpolation with computed barycentrics"};
        const Barycentric kind = known->first;
        for (const Site& site : first) {
            const Inst& inst = p_->blocks[site.block].insts[site.index];
            const Operand* operand = nullptr;
            std::optional<Value> read;
            if (const auto* intrp = std::get_if<VintrpFields>(&inst.fields)) {
                if (inst.name() == "v_interp_p1_f32") {
                    if (const auto* vec = std::get_if<VectorOperand>(&intrp->src)) {
                        operand = &intrp->attr;
                        read = inst.read(Reg::v(vec->reg));
                    }
                }
            } else if (const auto* val = std::get_if<Valu>(&inst.fields)) {
                if (inst.name() == "v_interp_p1ll_f16" && val->src.size() > 1) {
                    if (const auto* vec = std::get_if<VectorOperand>(&val->src[1])) {
                        operand = &val->src[0];
                        read = inst.read(Reg::v(vec->reg));
                    }
                }
            }
            if (operand == nullptr) throw IoError{"an interpolation without its first half"};
            if (!(*operand == attr)) throw IoError{"interpolation halves of different attributes"};
            const auto index = barycentric(read);
            if (!index || index->first != kind || index->second != 0)
                throw IoError{"an interpolation with computed barycentrics"};
        }
        return Pair{kind, std::move(first)};
    }

private:
    void sites(Value value, std::vector<Value>& seen, std::vector<Site>& out) const {
        if (const auto it = defs_->find(value); it != defs_->end()) {
            if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
            return;
        }
        const auto phi = phis_->find(value);
        if (phi == phis_->end()) throw IoError{"an interpolation without its first half"};
        if (std::find(seen.begin(), seen.end(), value) != seen.end()) return;
        seen.push_back(value);
        for (Value arg : p_->blocks[phi->second.block].phis[phi->second.index].args) sites(arg, seen, out);
    }

    const Program* p_;
    const std::unordered_map<Value, Site, ValueHash>* defs_;
    const std::unordered_map<Value, Site, ValueHash>* phis_;
};

const std::vector<VsOutput>& no_params() {
    static const std::vector<VsOutput> empty;
    return empty;
}

}  // namespace

std::vector<std::pair<std::uint8_t, Input>> pixel_input_vgprs(std::uint32_t input_addr) {
    return ps_vgprs(input_addr);
}

Input Input::simple(Kind kind) {
    Input input;
    input.kind = kind;
    return input;
}

Input Input::of_attribute(std::uint8_t row, std::uint8_t channel) {
    Input input;
    input.kind = Kind::Attribute;
    input.row = row;
    input.channel = channel;
    return input;
}

Input Input::of_barycentric(Barycentric bary, std::uint8_t index) {
    Input input;
    input.kind = Kind::Barycentric;
    input.bary = bary;
    input.index = index;
    return input;
}

Input Input::of_position(std::uint8_t channel) {
    Input input;
    input.kind = Kind::Position;
    input.index = channel;
    return input;
}

Input Input::of_group_id(std::uint8_t component) {
    Input input;
    input.kind = Kind::GroupId;
    input.index = component;
    return input;
}

Input Input::of_thread_id(std::uint8_t component) {
    Input input;
    input.kind = Kind::ThreadIdInGroup;
    input.index = component;
    return input;
}

Interp Interp::interpolated(Attribute attribute, Barycentric bary, std::vector<Site> first) {
    Interp interp;
    interp.kind = Kind::Interpolated;
    interp.attribute = attribute;
    interp.bary = bary;
    interp.first = std::move(first);
    return interp;
}

Interp Interp::vertex_value(Attribute attribute, std::uint8_t vertex) {
    Interp interp;
    interp.kind = Kind::Vertex;
    interp.attribute = attribute;
    interp.vertex = vertex;
    return interp;
}

Target Target::color(std::uint8_t index) {
    Target target;
    target.kind = Kind::Color;
    target.index = index;
    return target;
}

Target Target::depth() {
    Target target;
    target.kind = Kind::Depth;
    return target;
}

Target Target::null_target() {
    Target target;
    target.kind = Kind::Null;
    return target;
}

Target Target::position(std::uint8_t channel) {
    Target target;
    target.kind = Kind::Position;
    target.index = channel;
    return target;
}

Target Target::param(Usage usage, std::uint8_t row) {
    Target target;
    target.kind = Kind::Param;
    target.usage = usage;
    target.row = row;
    return target;
}

Target Target::unread() {
    Target target;
    target.kind = Kind::Unread;
    return target;
}

Target Target::primitive() {
    Target target;
    target.kind = Kind::Primitive;
    return target;
}

Target Target::primitive_param(Usage usage, std::uint8_t row) {
    Target target;
    target.kind = Kind::PrimitiveParam;
    target.usage = usage;
    target.row = row;
    return target;
}

Io raise(const Program& program, const NativeProgram& native) {
    std::vector<std::pair<Reg, Input>> loaded;
    const std::vector<PsInput>* ps_inputs = nullptr;
    const std::vector<VsOutput>* params = &no_params();
    if (native.stage == HwStage::Ps) {
        const auto* ps = std::get_if<PsIo>(&native.io);
        if (ps == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        for (const auto& entry : ps_vgprs(ps->input_addr)) loaded.emplace_back(Reg::v(entry.first), entry.second);
        ps_inputs = &ps->inputs;
    } else if (native.stage == HwStage::Vs) {
        const auto* vs = std::get_if<VsIo>(&native.io);
        if (vs == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        loaded.emplace_back(Reg::v(0), Input::simple(Input::Kind::VertexId));
        loaded.emplace_back(Reg::v(2), Input::simple(Input::Kind::PrimitiveId));
        loaded.emplace_back(Reg::v(3), Input::simple(Input::Kind::InstanceId));
        for (const VsInput& input : vs->inputs)
            for (std::uint8_t channel = 0; channel < 4; ++channel)
                if (((input.mask >> channel) & 1) != 0)
                    loaded.emplace_back(Reg::v(static_cast<std::uint8_t>(input.vgpr + channel)),
                                        Input::of_attribute(input.row, channel));
        params = &vs->outputs;
    } else if (native.stage == HwStage::Cs) {
        const auto* cs = std::get_if<ComputeIo>(&native.io);
        if (cs == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        const auto [ids, info] = cs->system_sgprs();
        for (std::uint8_t c = 0; c < cs->thread_id_vgprs(); ++c)
            loaded.emplace_back(Reg::v(c), Input::of_thread_id(c));
        for (std::size_t c = 0; c < ids.size(); ++c)
            if (ids[c])
                loaded.emplace_back(Reg::s(*ids[c]), Input::of_group_id(static_cast<std::uint8_t>(c)));
        if (info) loaded.emplace_back(Reg::s(*info), Input::simple(Input::Kind::GroupInfo));
    } else if (native.stage == HwStage::Ms) {
        const auto* ms = std::get_if<MeshIo>(&native.io);
        if (ms == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        std::optional<Value> group;
        for (const auto& entry : program.inputs)
            if (entry.first == Reg::v(5)) group = entry.second;
        for (const Block& block : program.blocks) {
            for (const Inst& inst : block.insts) {
                if (!group) continue;
                bool reads_group = false;
                for (const auto& entry : inst.reads)
                    if (entry.first == Reg::v(5) && entry.second == *group) reads_group = true;
                if (reads_group && inst.name() != "v_readfirstlane_b32" && !inst.write(Reg::v(5)))
                    detail::fail_unsupported(inst.machine.offset, "the mesh group input read per lane");
            }
        }
        loaded.emplace_back(Reg::v(5), Input::simple(Input::Kind::MeshGroup));
        loaded.emplace_back(Reg::s(3), Input::simple(Input::Kind::MergedWaveInfo));
        params = &ms->outputs;
    } else {
        detail::fail_unsupported(0, "I/O of this stage");
    }

    std::vector<std::pair<Value, Input>> inputs;
    for (const auto& entry : program.inputs)
        for (const auto& candidate : loaded)
            if (candidate.first == entry.first) {
                inputs.emplace_back(entry.second, candidate.second);
                break;
            }
    const auto input_of = [&](Value value) -> std::optional<Input> {
        for (const auto& entry : inputs)
            if (entry.first == value) return entry.second;
        return std::nullopt;
    };
    const auto attribute = [&](const Inst& inst, const Operand& attr) -> Attribute {
        const auto* a = std::get_if<AttributeOperand>(&attr);
        if (a == nullptr) detail::fail_unsupported(inst.machine.offset, "an interpolation operand");
        if (ps_inputs != nullptr)
            for (const PsInput& input : *ps_inputs)
                if (input.attr == a->index) return Attribute{input.usage, input.row, a->chan, a->high};
        detail::fail_unsupported(inst.machine.offset, "an interpolated attribute the stage data does not list");
    };
    const auto barycentric = [&](std::optional<Value> value)
        -> std::optional<std::pair<Barycentric, std::uint8_t>> {
        if (!value) return std::nullopt;
        const auto input = input_of(*value);
        if (!input || input->kind != Input::Kind::Barycentric) return std::nullopt;
        return std::pair<Barycentric, std::uint8_t>{input->bary, input->index};
    };

    Io io;
    io.inputs = inputs;
    if (const auto* cs = std::get_if<ComputeIo>(&native.io)) io.initialized_sgprs = cs->initialized_sgprs();
    std::unordered_map<Value, Site, ValueHash> defs;
    std::unordered_map<Value, Site, ValueHash> phis;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i)
            for (const auto& entry : block.insts[i].writes) defs[entry.second] = Site{b, i};
        for (std::size_t i = 0; i < block.phis.size(); ++i) phis[block.phis[i].value] = Site{b, i};
    }
    const Halves halves(program, defs, phis);

    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            const Site site{b, i};
            const std::size_t at = inst.machine.offset;
            const std::string_view name = inst.name();
            if (const auto* intrp = std::get_if<VintrpFields>(&inst.fields)) {
                const Attribute attr = attribute(inst, intrp->attr);
                if (name == "v_interp_mov_f32") {
                    const auto* c = std::get_if<ConstantOperand>(&intrp->src);
                    if (c == nullptr) detail::fail_unsupported(at, "an interpolation form");
                    io.interps.emplace_back(site, Interp::vertex_value(attr, c->code));
                } else if (name == "v_interp_p1_f32") {
                } else if (name == "v_interp_p2_f32") {
                    const auto* src = std::get_if<VectorOperand>(&intrp->src);
                    if (src == nullptr) detail::fail_unsupported(at, "an interpolation form");
                    const auto* dst = std::get_if<VectorOperand>(&intrp->dst);
                    if (dst == nullptr) detail::fail_unsupported(at, "an interpolation operand");
                    try {
                        const Pair pair = halves.pair(intrp->attr, inst.read(Reg::v(dst->reg)),
                                                      inst.read(Reg::v(src->reg)), barycentric);
                        io.interps.emplace_back(site, pair.with(attr));
                    } catch (const IoError& error) {
                        detail::fail_unsupported(at, error.what);
                    }
                } else {
                    detail::fail_unsupported(at, "an interpolation form");
                }
            } else if (const auto* val = std::get_if<Valu>(&inst.fields); val != nullptr && name.starts_with("v_interp_")) {
                if (name == "v_interp_p1ll_f16") {
                } else if (name == "v_interp_p2_f16") {
                    if (val->src.empty()) detail::fail_unsupported(at, "an interpolation form");
                    const Attribute attr = attribute(inst, val->src[0]);
                    const auto read = [&](std::size_t k) -> std::optional<Value> {
                        if (k >= val->src.size()) return std::nullopt;
                        const auto* vec = std::get_if<VectorOperand>(&val->src[k]);
                        if (vec == nullptr) return std::nullopt;
                        return inst.read(Reg::v(vec->reg));
                    };
                    try {
                        const Pair pair = halves.pair(val->src[0], read(2), read(1), barycentric);
                        io.interps.emplace_back(site, pair.with(attr));
                    } catch (const IoError& error) {
                        detail::fail_unsupported(at, error.what);
                    }
                } else {
                    detail::fail_unsupported(at, "an interpolation form");
                }
            } else if (const auto* exp = std::get_if<ExpFields>(&inst.fields)) {
                const std::uint8_t t = exp->target;
                Target target;
                if (t <= 7) {
                    target = Target::color(t);
                } else if (t == 8) {
                    target = Target::depth();
                } else if (t == 9) {
                    target = Target::null_target();
                } else if (t >= 12 && t <= 16) {
                    target = Target::position(static_cast<std::uint8_t>(t - 12));
                } else if (t == 20) {
                    target = Target::primitive();
                } else if (t == 32 && params->empty()) {
                    target = Target::unread();
                } else if (t >= 32 && t <= 63) {
                    const VsOutput* output = nullptr;
                    for (const VsOutput& candidate : *params)
                        if ((candidate.param & 0x7F) == static_cast<std::uint8_t>(t - 32)) {
                            output = &candidate;
                            break;
                        }
                    if (output == nullptr)
                        detail::fail_unsupported(at, "a parameter export the stage data does not list");
                    if ((output->param & 0x80) != 0)
                        target = Target::primitive_param(output->usage, output->row);
                    else
                        target = Target::param(output->usage, output->row);
                } else {
                    detail::fail_unsupported(at, "an export target");
                }
                std::array<std::optional<Value>, 4> values;
                for (std::size_t k = 0; k < exp->src.size(); ++k) {
                    if (!exp->src[k]) continue;
                    if (const auto* vec = std::get_if<VectorOperand>(&*exp->src[k]))
                        values[k] = inst.read(Reg::v(vec->reg));
                }
                Export export_value;
                export_value.site = site;
                export_value.target = target;
                export_value.values = values;
                export_value.compressed = exp->compr;
                export_value.done = exp->done;
                io.exports.push_back(export_value);
            }
        }
    }
    return io;
}

}  // namespace g6shim::shader
