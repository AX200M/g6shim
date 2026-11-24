// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/gfx10_sp3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g6shim::shader {

namespace {

std::string hex_lower(std::uint64_t value, int width) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llx", width, static_cast<unsigned long long>(value));
    return buf;
}

std::string hex_upper(std::uint64_t value, int width) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llX", width, static_cast<unsigned long long>(value));
    return buf;
}

std::string scalar_name(std::uint8_t code) {
    if (code <= 105) return "s" + std::to_string(code);
    switch (code) {
        case 106: return "vcc_lo";
        case 107: return "vcc_hi";
        case 124: return "m0";
        case 125: return "null";
        case 126: return "exec_lo";
        default: break;
    }
    if (code >= 108 && code <= 123) return "ttmp" + std::to_string(code - 108);
    return "exec_hi";
}

std::string const_name(std::uint8_t code) {
    if (code >= 128 && code <= 192) return std::to_string(code - 128);
    if (code >= 193 && code <= 208) return std::to_string(192 - code);
    switch (code) {
        case 235: return "src_shared_base";
        case 236: return "src_shared_limit";
        case 237: return "src_private_base";
        case 238: return "src_private_limit";
        case 239: return "src_pops_exiting_wave_id";
        case 240: return "0.5";
        case 241: return "-0.5";
        case 242: return "1.0";
        case 243: return "-1.0";
        case 244: return "2.0";
        case 245: return "-2.0";
        case 246: return "4.0";
        case 247: return "-4.0";
        case 248: return "0.15915494";
        case 251: return "src_vccz";
        case 252: return "src_execz";
        case 253: return "src_scc";
        case 254: return "src_lds_direct";
        default: return "<src " + std::to_string(code) + ">";
    }
}

template <class Range>
std::string join(const Range& parts) {
    std::string out;
    bool first = true;
    for (const std::string& part : parts) {
        if (!first) out += ", ";
        out += part;
        first = false;
    }
    return out;
}

std::string label(const Instruction& inst, std::uint16_t imm) {
    const std::int64_t target = static_cast<std::int64_t>(inst.offset) +
                                4 * static_cast<std::int64_t>(inst.words.size()) +
                                4 * static_cast<std::int64_t>(static_cast<std::int16_t>(imm));
    return "label_" + hex_upper(static_cast<std::uint64_t>(target), 4);
}

std::string hwreg(std::uint16_t imm) {
    const std::uint16_t id = imm & 0x3F;
    const std::uint16_t offset = (imm >> 6) & 0x1F;
    const std::uint16_t size = static_cast<std::uint16_t>(((imm >> 11) & 0x1F) + 1);
    std::string name;
    switch (id) {
        case 1: name = "HW_REG_MODE"; break;
        case 20: name = "HW_REG_SHADER_FLAT_SCRATCH_LO"; break;
        case 21: name = "HW_REG_SHADER_FLAT_SCRATCH_HI"; break;
        case 25: name = "HW_REG_POPS_PACKER"; break;
        default: name = std::to_string(id); break;
    }
    if (offset == 0 && size == 32) return "hwreg(" + name + ")";
    return "hwreg(" + name + ", " + std::to_string(offset) + ", " + std::to_string(size) + ")";
}

std::string waitcnt(std::uint16_t imm) {
    const std::uint32_t vm = static_cast<std::uint32_t>(imm & 0xF) | static_cast<std::uint32_t>(imm >> 14) << 4;
    const std::uint32_t exp = (imm >> 4) & 7;
    const std::uint32_t lgkm = (imm >> 8) & 0x3F;
    std::vector<std::string> parts;
    if (vm != 0x3F) parts.push_back("vmcnt(" + std::to_string(vm) + ")");
    if (exp != 7) parts.push_back("expcnt(" + std::to_string(exp) + ")");
    if (lgkm != 0x3F) parts.push_back("lgkmcnt(" + std::to_string(lgkm) + ")");
    if (parts.empty() || parts.size() == 3) return "0x" + hex_lower(imm, 4);
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += " & ";
        out += parts[i];
    }
    return out;
}

std::string sendmsg(std::uint16_t imm) {
    const std::uint16_t msg = imm & 0xF;
    const std::uint16_t op = (imm >> 4) & 7;
    const std::uint16_t stream = (imm >> 8) & 3;
    std::string msg_name;
    switch (msg) {
        case 2: msg_name = "MSG_GS"; break;
        case 3: msg_name = "MSG_GS_DONE"; break;
        case 7: msg_name = "MSG_ORDERED_PS_DONE"; break;
        case 9: msg_name = "MSG_GS_ALLOC_REQ"; break;
        default: msg_name = std::to_string(msg); break;
    }
    constexpr std::array<std::string_view, 4> kGsOps{"GS_OP_NOP", "GS_OP_CUT", "GS_OP_EMIT", "GS_OP_EMIT_CUT"};
    const std::string op_name =
        static_cast<std::size_t>(op) < kGsOps.size() ? std::string(kGsOps[op]) : std::to_string(op);
    if (msg == 3 && op == 0) return "sendmsg(" + msg_name + ", " + op_name + ")";
    return "sendmsg(" + msg_name + ", " + op_name + ", " + std::to_string(stream) + ")";
}

std::string buffer_format(std::uint8_t format) {
    constexpr std::array<std::string_view, 78> kNames{{
        "INVALID",          "8_UNORM",          "8_SNORM",          "8_USCALED",
        "8_SSCALED",        "8_UINT",           "8_SINT",           "16_UNORM",
        "16_SNORM",         "16_USCALED",       "16_SSCALED",       "16_UINT",
        "16_SINT",          "16_FLOAT",         "8_8_UNORM",        "8_8_SNORM",
        "8_8_USCALED",      "8_8_SSCALED",      "8_8_UINT",         "8_8_SINT",
        "32_UINT",          "32_SINT",          "32_FLOAT",         "16_16_UNORM",
        "16_16_SNORM",      "16_16_USCALED",    "16_16_SSCALED",    "16_16_UINT",
        "16_16_SINT",       "16_16_FLOAT",      "10_11_11_UNORM",   "10_11_11_SNORM",
        "10_11_11_USCALED", "10_11_11_SSCALED", "10_11_11_UINT",    "10_11_11_SINT",
        "10_11_11_FLOAT",   "11_11_10_UNORM",   "11_11_10_SNORM",   "11_11_10_USCALED",
        "11_11_10_SSCALED", "11_11_10_UINT",    "11_11_10_SINT",    "11_11_10_FLOAT",
        "10_10_10_2_UNORM", "10_10_10_2_SNORM", "10_10_10_2_USCALED", "10_10_10_2_SSCALED",
        "10_10_10_2_UINT",  "10_10_10_2_SINT",  "2_10_10_10_UNORM", "2_10_10_10_SNORM",
        "2_10_10_10_USCALED", "2_10_10_10_SSCALED", "2_10_10_10_UINT", "2_10_10_10_SINT",
        "8_8_8_8_UNORM",    "8_8_8_8_SNORM",    "8_8_8_8_USCALED",  "8_8_8_8_SSCALED",
        "8_8_8_8_UINT",     "8_8_8_8_SINT",     "32_32_UINT",       "32_32_SINT",
        "32_32_FLOAT",      "16_16_16_16_UNORM", "16_16_16_16_SNORM", "16_16_16_16_USCALED",
        "16_16_16_16_SSCALED", "16_16_16_16_UINT", "16_16_16_16_SINT", "16_16_16_16_FLOAT",
        "32_32_32_UINT",    "32_32_32_SINT",    "32_32_32_FLOAT",   "32_32_32_32_UINT",
        "32_32_32_32_SINT", "32_32_32_32_FLOAT",
    }};
    if (static_cast<std::size_t>(format) < kNames.size()) return "BUF_FMT_" + std::string(kNames[format]);
    return std::to_string(format);
}

void flags(std::string& out, const Cache& cache, bool dlc_first) {
    const auto add = [&](bool on, std::string_view text) {
        if (on) out += text;
    };
    if (dlc_first) add(cache.dlc, " dlc");
    add(cache.glc, " glc");
    add(cache.slc, " slc");
    if (!dlc_first) add(cache.dlc, " dlc");
    if (!out.empty() && out.front() == ' ') out.erase(out.begin());
}

bool bit_at(std::uint8_t mask, std::size_t index) { return ((mask >> index) & 1) != 0; }

std::string valu(std::string_view name, const Valu& v, Encoding encoding) {
    const bool mix = name.starts_with("v_fma_mix");
    const auto source_text = [&](std::size_t i) {
        std::string s = i < v.src.size() ? operand_text(v.src[i]) : std::string{};
        if (v.sdwa && bit_at(v.sdwa->sext, i)) s = "sext(" + s + ")";
        if (encoding == Encoding::Vop3p) {
            if (mix) {
                if (bit_at(v.opsel_hi, i))
                    s = std::string(bit_at(v.opsel, i) ? "sel_hi(" : "sel_lo(") + s + ")";
                if (bit_at(v.neg_hi, i)) s = "abs(" + s + ")";
                if (bit_at(v.neg, i)) s = "-" + s;
            } else {
                if (bit_at(v.neg, i) && bit_at(v.neg_hi, i)) s = "-" + s;
                const bool sel = bit_at(v.opsel, i);
                const bool hi = bit_at(v.opsel_hi, i);
                if (!sel && !hi) s = "sel_lo(" + s + ")";
                else if (sel && hi) s = "sel_hi(" + s + ")";
                else if (sel) s = "sel_hi_lo(" + s + ")";
            }
        } else {
            if (bit_at(v.abs, i)) s = "abs(" + s + ")";
            if (bit_at(v.neg, i)) s = "-" + s;
            if (bit_at(v.opsel, i) && !name.starts_with("v_permlane")) s = "sel_hi(" + s + ")";
        }
        return s;
    };
    std::vector<std::string> ops;
    if (v.dst) ops.push_back(operand_text(*v.dst));
    if (v.carry) ops.push_back(operand_text(*v.carry));
    std::vector<std::size_t> order(v.src.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (name.starts_with("v_interp_") && order.size() > 1) std::swap(order[0], order[1]);
    for (const std::size_t i : order) ops.push_back(source_text(i));
    std::string out = join(ops);
    const auto suffix = [&](std::string_view text) { out += ' '; out += text; };
    if (!v.src.empty()) {
        const auto* attr = std::get_if<AttributeOperand>(&v.src.front());
        if (attr != nullptr && attr->high) suffix("high");
    }
    constexpr std::array<std::string_view, 4> kOmod{"", "mul:2", "mul:4", "div:2"};
    const std::string_view omod = kOmod[v.omod & 3];
    if (name.starts_with("v_permlane")) {
        if ((v.opsel & 1) != 0) suffix("fi:1");
        if ((v.opsel & 2) != 0) suffix("bound_ctrl:1");
    }
    if (v.sdwa) {
        if (v.clamp) suffix("clamp");
        if (!omod.empty()) suffix(omod);
        constexpr std::array<std::string_view, 8> kSel{"BYTE_0", "BYTE_1", "BYTE_2", "BYTE_3",
                                                       "WORD_0", "WORD_1", "DWORD", "?"};
        if (v.sdwa->dst_sel != 6) suffix("dst_sel:" + std::string(kSel[v.sdwa->dst_sel & 7]));
        if (v.sdwa->dst_unused != 0) {
            constexpr std::array<std::string_view, 4> kUnused{"UNUSED_PAD", "UNUSED_SEXT", "UNUSED_PRESERVE", "?"};
            suffix("dst_unused:" + std::string(kUnused[v.sdwa->dst_unused & 3]));
        }
        for (std::size_t i = 0; i < v.sdwa->src_sel.size(); ++i) {
            const std::uint8_t sel = v.sdwa->src_sel[i];
            if (sel != 6) suffix("src" + std::to_string(i) + "_sel:" + std::string(kSel[sel & 7]));
        }
    } else {
        if (!omod.empty()) suffix(omod);
        if (v.clamp) suffix("clamp");
    }
    if (v.dpp) {
        if (const auto* row = std::get_if<DppRow>(&*v.dpp)) {
            const std::uint16_t ctrl = row->ctrl;
            std::string text;
            if (ctrl <= 0x0FF)
                text = "quad_perm:[" + std::to_string(ctrl & 3) + "," + std::to_string((ctrl >> 2) & 3) + "," +
                       std::to_string((ctrl >> 4) & 3) + "," + std::to_string((ctrl >> 6) & 3) + "]";
            else if (ctrl >= 0x101 && ctrl <= 0x10F)
                text = "row_shl:" + std::to_string(ctrl & 0xF);
            else if (ctrl >= 0x111 && ctrl <= 0x11F)
                text = "row_shr:" + std::to_string(ctrl & 0xF);
            else if (ctrl >= 0x121 && ctrl <= 0x12F)
                text = "row_ror:" + std::to_string(ctrl & 0xF);
            else if (ctrl == 0x140)
                text = "row_mirror";
            else if (ctrl == 0x141)
                text = "row_half_mirror";
            else if (ctrl >= 0x150 && ctrl <= 0x15F)
                text = "row_share:" + std::to_string(ctrl & 0xF);
            else if (ctrl >= 0x160 && ctrl <= 0x16F)
                text = "row_xmask:" + std::to_string(ctrl & 0xF);
            else
                text = "dpp_ctrl:0x" + hex_lower(ctrl, 1);
            suffix(text);
            if (row->row_mask != 0xF) suffix("row_mask:0x" + hex_lower(row->row_mask, 1));
            if (row->bank_mask != 0xF) suffix("bank_mask:0x" + hex_lower(row->bank_mask, 1));
            if (row->bound_ctrl) suffix("bound_ctrl:1");
            if (row->fi) suffix("fi:1");
        } else {
            const auto& lanes = std::get<DppLanes>(*v.dpp);
            std::string text = "dpp8:[";
            for (std::size_t i = 0; i < 8; ++i) {
                if (i != 0) text += ',';
                text += std::to_string((lanes.sel >> (3 * i)) & 7);
            }
            text += ']';
            suffix(text);
            if (lanes.fi) suffix("fi:1");
        }
    }
    return out;
}

bool is_scalar_null(const Operand& operand) {
    const auto* scalar = std::get_if<ScalarOperand>(&operand);
    return scalar != nullptr && scalar->code == kNull;
}

std::string operands(const Instruction& inst, std::string_view name, const Fields& fields) {
    std::string out;
    if (const auto* sop = std::get_if<SopFields>(&fields)) {
        std::vector<std::string> regs;
        if (sop->dst) regs.push_back(operand_text(*sop->dst));
        for (const Operand& operand : sop->src) regs.push_back(operand_text(operand));
        const std::string joined = join(regs);
        if (name == "s_getreg_b32" && sop->imm) {
            out = joined + ", " + hwreg(*sop->imm);
        } else if ((name == "s_setreg_b32" || name == "s_setreg_imm32_b32") && sop->imm) {
            out = hwreg(*sop->imm) + ", " + joined;
        } else if (name == "s_call_b64" && sop->imm) {
            out = joined + ", " + label(inst, *sop->imm);
        } else if (sop->imm && joined.empty()) {
            out = "0x" + hex_lower(*sop->imm, 4);
        } else if (sop->imm) {
            out = joined + ", 0x" + hex_lower(*sop->imm, 4);
        } else {
            out = joined;
        }
    } else if (const auto* sopp = std::get_if<SoppFields>(&fields)) {
        const std::uint16_t imm = sopp->imm;
        if (name == "s_endpgm" || name == "s_barrier" || name == "s_code_end" || name == "s_endpgm_saved" ||
            name == "s_endpgm_ordered_ps_done" || name == "s_wakeup" || name == "s_icache_inv" ||
            name == "s_ttracedata") {
            out.clear();
        } else if (name == "s_branch" || name.starts_with("s_cbranch_")) {
            out = label(inst, imm);
        } else if (name == "s_waitcnt") {
            out = waitcnt(imm);
        } else if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            out = sendmsg(imm);
        } else {
            out = "0x" + hex_lower(imm, 4);
        }
    } else if (const auto* smem = std::get_if<SmemFields>(&fields)) {
        if (smem->data) {
            std::string offset_text;
            if (is_scalar_null(smem->soffset) && smem->offset == 0) {
                offset_text = operand_text(smem->soffset);
            } else if (is_scalar_null(smem->soffset)) {
                offset_text = "0x" + hex_lower(static_cast<std::uint32_t>(smem->offset) & 0x1FFFFF, 6);
            } else if (smem->offset == 0) {
                offset_text = operand_text(smem->soffset);
            } else {
                offset_text = operand_text(smem->soffset) + " offset:0x" +
                              hex_lower(static_cast<std::uint32_t>(smem->offset) & 0x1FFFFF, 6);
            }
            out = operand_text(*smem->data) + ", " + operand_text(smem->base) + ", " + offset_text;
        }
        flags(out, smem->cache, false);
    } else if (const auto* v = std::get_if<Valu>(&fields)) {
        out = valu(name, *v, inst.encoding);
    } else if (const auto* vintrp = std::get_if<VintrpFields>(&fields)) {
        std::string src = operand_text(vintrp->src);
        if (name == "v_interp_mov_f32") {
            if (const auto* p = std::get_if<ConstantOperand>(&vintrp->src)) {
                constexpr std::array<std::string_view, 3> kParams{"p10", "p20", "p0"};
                src = static_cast<std::size_t>(p->code) < kParams.size() ? std::string(kParams[p->code]) : "p?";
            }
        }
        out = operand_text(vintrp->dst) + ", " + src + ", " + operand_text(vintrp->attr);
    } else if (const auto* ds = std::get_if<DsFields>(&fields)) {
        std::vector<std::string> parts;
        if (ds->dst) parts.push_back(operand_text(*ds->dst));
        if (ds->addr) parts.push_back(operand_text(*ds->addr));
        for (const Operand& operand : ds->data) parts.push_back(operand_text(operand));
        out = join(parts);
        if (ds_pair(name)) {
            if (ds->offset0 != 0) out += " offset0:" + std::to_string(ds->offset0);
            if (ds->offset1 != 0) out += " offset1:" + std::to_string(ds->offset1);
        } else {
            const std::uint32_t offset = static_cast<std::uint32_t>(ds->offset1) << 8 | ds->offset0;
            if (offset != 0) out += " offset:" + std::to_string(offset);
        }
        if (ds->gds) out += " gds";
    } else if (const auto* buffer = std::get_if<BufferFields>(&fields)) {
        if (buffer->data) {
            out = operand_text(*buffer->data) + ", " + operand_text(buffer->addr) + ", " +
                  operand_text(buffer->rsrc) + ", " + operand_text(buffer->soffset);
            if (buffer->idxen) out += " idxen";
            if (buffer->offen) out += " offen";
            if (buffer->offset != 0) out += " offset:" + std::to_string(buffer->offset);
            if (buffer->format) out += " format:[" + buffer_format(*buffer->format) + "]";
        }
        flags(out, buffer->cache, false);
        if (buffer->lds) out += " lds";
        if (buffer->tfe) out += " tfe";
    } else if (const auto* image = std::get_if<ImageFields>(&fields)) {
        std::string addr;
        if (image->addr.size() == 1) {
            addr = operand_text(image->addr.front());
        } else {
            std::string compact;
            for (std::size_t i = 0; i < image->addr.size(); ++i) {
                if (i != 0) compact += ',';
                compact += operand_text(image->addr[i]);
            }
            addr = "[" + compact + "]";
        }
        std::vector<std::string> parts{operand_text(image->data), addr, operand_text(image->rsrc)};
        if (image->sampler) parts.push_back(operand_text(*image->sampler));
        out = join(parts);
        if (image->dmask != 1) out += " dmask:0x" + hex_lower(image->dmask, 1);
        constexpr std::array<std::string_view, 8> kDims{"1D",  "2D",  "3D",  "CUBE",
                                                        "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY"};
        out += " dim:SQ_RSRC_IMG_";
        out += kDims[image->dim & 7];
        if (image->r128) out += " r128";
        if (image->unorm) out += " unorm";
        flags(out, image->cache, true);
        if (image->a16) out += " a16";
        if (image->d16) out += " d16";
        if (image->tfe) out += " tfe";
        if (image->lwe) out += " lwe";
    } else if (const auto* flat = std::get_if<FlatFields>(&fields)) {
        std::vector<std::string> parts;
        if (flat->dst) parts.push_back(operand_text(*flat->dst));
        parts.push_back(flat->addr ? operand_text(*flat->addr) : std::string("off"));
        if (flat->data) parts.push_back(operand_text(*flat->data));
        if (flat->saddr) parts.push_back(operand_text(*flat->saddr));
        out = join(parts);
        if (flat->offset != 0) out += " inst_offset:" + std::to_string(flat->offset);
        flags(out, flat->cache, false);
        if (flat->lds) out += " lds";
    } else if (const auto* exp = std::get_if<ExpFields>(&fields)) {
        const std::uint8_t target = exp->target;
        std::string target_text;
        if (target <= 7)
            target_text = "mrt" + std::to_string(target);
        else if (target == 8)
            target_text = "mrtz";
        else if (target == 9)
            target_text = "null";
        else if (target >= 12 && target <= 15)
            target_text = "pos" + std::to_string(target - 12);
        else if (target == 20)
            target_text = "prim";
        else if (target >= 32 && target <= 63)
            target_text = "param" + std::to_string(target - 32);
        else
            target_text = "<target " + std::to_string(target) + ">";
        std::vector<std::string> parts{target_text};
        for (const auto& src : exp->src) parts.push_back(src ? operand_text(*src) : std::string("off"));
        out = join(parts);
        if (exp->done) out += " done";
        if (exp->compr) out += " compr";
        if (exp->vm) out += " vm";
    }
    return out;
}

}  // namespace

std::string operand_text(const Operand& operand) {
    if (const auto* scalar = std::get_if<ScalarOperand>(&operand)) {
        if (scalar->dwords == 1) return scalar_name(scalar->code);
        if (scalar->code == 106 && scalar->dwords == 2) return "vcc";
        if (scalar->code == 126 && scalar->dwords == 2) return "exec";
        if (scalar->code < 106)
            return "s[" + std::to_string(scalar->code) + ":" +
                   std::to_string(scalar->code + static_cast<std::uint32_t>(scalar->dwords) - 1) + "]";
        if (scalar->code >= 108 && scalar->code <= 123)
            return "ttmp[" + std::to_string(scalar->code - 108) + ":" +
                   std::to_string(scalar->code - 108 + static_cast<std::uint32_t>(scalar->dwords) - 1) + "]";
        return scalar_name(scalar->code);
    }
    if (const auto* vec = std::get_if<VectorOperand>(&operand)) {
        if (vec->dwords == 1) return "v" + std::to_string(vec->reg);
        return "v[" + std::to_string(vec->reg) + ":" +
               std::to_string(static_cast<std::uint32_t>(vec->reg) + vec->dwords - 1) + "]";
    }
    if (const auto* constant = std::get_if<ConstantOperand>(&operand)) return const_name(constant->code);
    if (const auto* literal = std::get_if<LiteralOperand>(&operand)) return "lit(0x" + hex_lower(literal->value, 8) + ")";
    const auto& attr = std::get<AttributeOperand>(operand);
    constexpr std::array<std::string_view, 4> kChannels{"x", "y", "z", "w"};
    return "attr" + std::to_string(attr.index) + "." + std::string(kChannels[attr.chan & 3]);
}

std::string Instruction::text(bool wave64) const {
    const std::string_view name = this->name().value_or(std::string_view{"?"});
    const std::string ops = operands(*this, name, this->fields(wave64));
    if (ops.empty()) return std::string(name);
    return std::string(name) + " " + ops;
}

}  // namespace g6shim::shader
