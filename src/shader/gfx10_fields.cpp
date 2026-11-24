// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/gfx10.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <string_view>
#include <vector>

namespace g6shim::shader {

namespace {

constexpr std::array<std::string_view, 3> kPairFamilies{"read2", "write2", "wrxchg2"};

constexpr std::array<std::string_view, 45> kVop3TwoSources{
    "v_add_f64",         "v_mul_f64",           "v_min_f64",           "v_max_f64",
    "v_ldexp_f64",       "v_mul_lo_u32",        "v_mul_hi_u32",        "v_mul_lo_i32",
    "v_mul_hi_i32",      "v_trig_preop_f64",    "v_lshlrev_b64",       "v_lshrrev_b64",
    "v_ashrrev_i64",     "v_add_nc_u16",        "v_sub_nc_u16",        "v_mul_lo_u16",
    "v_lshrrev_b16",     "v_ashrrev_i16",       "v_max_u16",           "v_max_i16",
    "v_min_u16",         "v_min_i16",           "v_add_nc_i16",        "v_sub_nc_i16",
    "v_add_co_u32",      "v_sub_co_u32",        "v_subrev_co_u32",     "v_pack_b32_f16",
    "v_cvt_pknorm_i16_f16", "v_cvt_pknorm_u16_f16", "v_lshlrev_b16",   "v_interp_p1ll_f16",
    "v_readlane_b32",    "v_writelane_b32",     "v_ldexp_f32",         "v_bfm_b32",
    "v_bcnt_u32_b32",    "v_mbcnt_lo_u32_b32",  "v_mbcnt_hi_u32_b32",  "v_cvt_pknorm_i16_f32",
    "v_cvt_pknorm_u16_f32", "v_cvt_pk_u16_u32", "v_cvt_pk_i16_i32",    "v_sub_nc_i32",
    "v_add_nc_i32",
};

constexpr std::array<std::string_view, 10> kVop3b{
    "v_add_co_u32",        "v_sub_co_u32",     "v_subrev_co_u32",  "v_add_co_ci_u32", "v_sub_co_ci_u32",
    "v_subrev_co_ci_u32",  "v_div_scale_f32",  "v_div_scale_f64",  "v_mad_u64_u32",   "v_mad_i64_i32",
};

bool listed(const auto& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool bit(std::uint32_t word, unsigned index) { return ((word >> index) & 1) != 0; }

bool contains(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    for (;;) {
        const std::size_t at = text.find(separator);
        if (at == std::string_view::npos) {
            out.push_back(text);
            return out;
        }
        out.push_back(text.substr(0, at));
        text.remove_prefix(at + 1);
    }
}

std::uint8_t as_u8(std::uint32_t value) { return static_cast<std::uint8_t>(value); }

std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
    const std::uint32_t sign = std::uint32_t{1} << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

Operand scalar(std::uint8_t code, std::uint8_t dwords) { return Operand{ScalarOperand{code, dwords}}; }

Operand vector(std::uint32_t reg, std::uint8_t dwords) { return Operand{VectorOperand{as_u8(reg), dwords}}; }

Operand constant(std::uint32_t code) { return Operand{ConstantOperand{as_u8(code)}}; }

Operand literal(std::uint32_t value) { return Operand{LiteralOperand{value}}; }

Operand attribute(std::uint32_t index, std::uint32_t chan, bool high) {
    return Operand{AttributeOperand{as_u8(index), as_u8(chan), high}};
}

Operand source(std::uint32_t code, std::uint8_t dwords, std::uint32_t literal_word) {
    if (code <= 127) return scalar(as_u8(code), dwords);
    if (code == 255) return literal(literal_word);
    if (code >= 256) return vector(code - 256, dwords);
    return constant(code);
}

std::uint8_t type_dwords(std::string_view token) {
    return token == "b64" || token == "u64" || token == "i64" || token == "f64" ? 2 : 1;
}

std::string_view last_token(std::string_view name) {
    const std::vector<std::string_view> parts = split(name, '_');
    return parts.back();
}

std::array<std::uint8_t, 3> salu_dwords(std::string_view name) {
    if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" || name == "s_bfe_u64" ||
        name == "s_bfe_i64" || name == "s_bitcmp0_b64" || name == "s_bitcmp1_b64")
        return {2, 2, 1};
    if (name == "s_bfm_b64" || name == "s_bitset0_b64" || name == "s_bitset1_b64" || name == "s_bitreplicate_b64_b32")
        return {2, 1, 1};
    if (name == "s_bcnt0_i32_b64" || name == "s_bcnt1_i32_b64" || name == "s_ff0_i32_b64" || name == "s_ff1_i32_b64" ||
        name == "s_flbit_i32_b64" || name == "s_flbit_i32_i64")
        return {1, 2, 2};
    const std::uint8_t n = type_dwords(last_token(name));
    return {n, n, n};
}

std::pair<std::uint8_t, std::array<std::uint8_t, 3>> valu_dwords(std::string_view name) {
    if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") return {2, {1, 2, 2}};
    if (name == "v_ldexp_f64" || name == "v_trig_preop_f64" || name == "v_cmp_class_f64" || name == "v_cmpx_class_f64")
        return {2, {2, 1, 2}};
    if (name == "v_mad_u64_u32" || name == "v_mad_i64_i32") return {2, {1, 1, 2}};
    if (name == "v_qsad_pk_u16_u8" || name == "v_mqsad_pk_u16_u8") return {2, {2, 1, 2}};
    if (name == "v_mqsad_u32_u8") return {4, {2, 1, 4}};
    if (name == "v_frexp_exp_i32_f64") return {1, {2, 2, 2}};
    const std::vector<std::string_view> parts = split(name, '_');
    if (parts.size() == 4 && parts[1] == "cvt") {
        const std::uint8_t n = type_dwords(parts[3]);
        return {type_dwords(parts[2]), {n, n, n}};
    }
    const std::uint8_t n = type_dwords(parts.back());
    return {n, {n, n, n}};
}

std::uint8_t data_dwords(std::string_view name) {
    std::uint8_t count = 1;
    for (const std::string_view token : split(name, '_')) {
        if (token == "dwordx2" || token == "x2" || token == "xy" || token == "b64" || token == "u64" || token == "i64" ||
            token == "f64")
            count = 2;
        else if (token == "dwordx3" || token == "xyz" || token == "b96")
            count = 3;
        else if (token == "dwordx4" || token == "xyzw" || token == "b128")
            count = 4;
        else if (token == "dwordx8")
            count = 8;
        else if (token == "dwordx16")
            count = 16;
    }
    if (contains(name, "cmpswap")) count = static_cast<std::uint8_t>(count * 2);
    if (contains(name, "format_d16")) count = static_cast<std::uint8_t>((count + 1) / 2);
    return count;
}

std::uint8_t image_addr_dwords(std::string_view name, std::uint8_t dim, bool a16) {
    struct ImageDim {
        std::uint8_t coords;
        std::uint8_t grads;
    };
    constexpr std::array<ImageDim, 8> kDims{{
        {1, 1},
        {2, 2},
        {3, 3},
        {3, 2},
        {2, 1},
        {3, 2},
        {3, 2},
        {4, 2},
    }};
    const auto [coords, grads] = kDims[dim & 7];
    if (name == "image_bvh_intersect_ray") return a16 ? 8 : 11;
    if (name == "image_bvh64_intersect_ray") return a16 ? 9 : 12;
    if (name == "image_get_resinfo") return 1;
    std::uint8_t whole = 0;
    std::uint8_t derivs = 0;
    std::uint8_t packed = coords;
    std::string_view rest;
    if (name.starts_with("image_sample"))
        rest = name.substr(std::string_view{"image_sample"}.size());
    else if (name.starts_with("image_gather4"))
        rest = name.substr(std::string_view{"image_gather4"}.size());
    if (!rest.empty()) {
        for (const std::string_view token : split(rest, '_')) {
            if (token == "o" || token == "b" || token == "c")
                whole = static_cast<std::uint8_t>(whole + 1);
            else if (token == "d" || token == "cd")
                derivs = grads;
            else if (token == "l" || token == "cl")
                packed = static_cast<std::uint8_t>(packed + 1);
        }
    } else if (contains(name, "_mip")) {
        packed = static_cast<std::uint8_t>(packed + 1);
    }
    if (a16) return static_cast<std::uint8_t>(whole + 2 * ((derivs + 1) / 2) + (packed + 1) / 2);
    return static_cast<std::uint8_t>(whole + 2 * derivs + packed);
}

Operand valu_dst(std::string_view name, std::uint32_t field, std::uint8_t dwords) {
    if (name == "v_readlane_b32" || name == "v_readfirstlane_b32") return scalar(as_u8(field), 1);
    return vector(field, dwords);
}

Valu valu_field(const Instruction& inst, std::string_view name, std::uint8_t lanes) {
    if (name == "v_nop" || name == "v_pipeflush" || name == "v_clrexcp") return {};
    const auto w = [&](std::size_t index) { return index < inst.words.size() ? inst.words[index] : 0u; };
    const std::uint32_t w0 = w(0);
    const auto [dst_n, src_n] = valu_dwords(name);
    const bool compare = inst.encoding == Encoding::Vopc || (inst.encoding == Encoding::Vop3 && inst.opcode < 0x100);
    const bool vop3b = listed(kVop3b, name);
    const bool carry_in = name.ends_with("_co_ci_u32") || name == "v_cndmask_b32";
    Valu v;
    if (inst.encoding == Encoding::Vop1 || inst.encoding == Encoding::Vop2 || inst.encoding == Encoding::Vopc) {
        const std::uint32_t ext = w(1);
        std::uint32_t src0 = w0 & 0x1FF;
        const std::uint32_t vsrc1 = 256 + ((w0 >> 9) & 0xFF);
        std::uint32_t src1 = vsrc1;
        if (src0 == 0xF9) {
            Sdwa sdwa;
            sdwa.dst_sel = as_u8((ext >> 8) & 7);
            sdwa.dst_unused = as_u8((ext >> 11) & 3);
            sdwa.src_sel = {as_u8((ext >> 16) & 7), as_u8((ext >> 24) & 7)};
            sdwa.sext = as_u8(((ext >> 19) & 1) | ((ext >> 26) & 2));
            src0 = (ext & 0xFF) | (bit(ext, 23) ? 0 : 256);
            if (bit(ext, 31)) src1 = vsrc1 - 256;
            v.neg = as_u8(((ext >> 20) & 1) | ((ext >> 27) & 2));
            v.abs = as_u8(((ext >> 21) & 1) | ((ext >> 28) & 2));
            if (compare) {
                if (bit(ext, 15)) v.dst = scalar(as_u8((ext >> 8) & 0x7F), lanes);
                sdwa.dst_sel = 6;
                sdwa.dst_unused = 0;
                v.sdwa = sdwa;
            } else {
                v.clamp = bit(ext, 13);
                v.omod = as_u8((ext >> 14) & 3);
                v.sdwa = sdwa;
            }
        } else if (src0 == 0xFA) {
            src0 = 256 + (ext & 0xFF);
            v.neg = as_u8(((ext >> 20) & 1) | ((ext >> 21) & 2));
            v.abs = as_u8(((ext >> 21) & 1) | ((ext >> 22) & 2));
            DppRow row;
            row.ctrl = static_cast<std::uint16_t>((ext >> 8) & 0x1FF);
            row.row_mask = as_u8(ext >> 28);
            row.bank_mask = as_u8((ext >> 24) & 0xF);
            row.bound_ctrl = bit(ext, 19);
            row.fi = bit(ext, 18);
            v.dpp = Dpp{row};
        } else if (src0 == 0xE9 || src0 == 0xEA) {
            v.dpp = Dpp{DppLanes{ext >> 8, src0 == 0xEA}};
            src0 = 256 + (ext & 0xFF);
        }
        const Operand op0 = source(src0, src_n[0], ext);
        const Operand op1 = source(src1, src_n[1], ext);
        if (inst.encoding == Encoding::Vopc) {
            const std::uint8_t mask = name.starts_with("v_cmpx") ? kExec : kVcc;
            if (!v.dst) v.dst = scalar(mask, lanes);
            v.src = {op0, op1};
        } else if (inst.encoding == Encoding::Vop1) {
            v.dst = valu_dst(name, (w0 >> 17) & 0xFF, dst_n);
            v.src = {op0};
        } else {
            v.dst = vector((w0 >> 17) & 0xFF, dst_n);
            if (name.ends_with("mk_f32") || name.ends_with("mk_f16"))
                v.src = {op0, literal(ext), op1};
            else if (name.ends_with("ak_f32") || name.ends_with("ak_f16"))
                v.src = {op0, op1, literal(ext)};
            else
                v.src = {op0, op1};
            if (carry_in) v.src.push_back(scalar(kVcc, lanes));
            if (name.ends_with("_co_ci_u32")) v.carry = scalar(kVcc, lanes);
        }
        return v;
    }
    const std::uint32_t w1 = w(1);
    const auto field = [&](std::size_t index) { return (w1 >> (9 * index)) & 0x1FF; };
    std::size_t count = 3;
    if (inst.encoding == Encoding::Vop3p) {
        count = name.starts_with("v_pk_") && !contains(name, "_fma_") && !contains(name, "_mad_") ? 2 : 3;
    } else if (inst.opcode < 0x100) {
        count = 2;
    } else if (inst.opcode < 0x140) {
        count = 2 + static_cast<std::size_t>(carry_in ? 1 : 0);
    } else if (inst.opcode >= 0x180 && inst.opcode < 0x200) {
        count = 1;
    } else if (listed(kVop3TwoSources, name)) {
        count = 2;
    }
    v.src.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t code = field(i);
        if (i == 0 && name.starts_with("v_interp_"))
            v.src.push_back(attribute(code & 0x3F, (code >> 6) & 3, bit(code, 8)));
        else if (i == 2 && carry_in)
            v.src.push_back(source(code, lanes, w(2)));
        else
            v.src.push_back(source(code, src_n[i], w(2)));
    }
    v.clamp = bit(w0, 15);
    v.neg = as_u8(w1 >> 29);
    if (inst.encoding == Encoding::Vop3p) {
        v.neg_hi = as_u8((w0 >> 8) & 7);
        v.opsel = as_u8((w0 >> 11) & 7);
        v.opsel_hi = as_u8(((w1 >> 27) & 3) | ((w0 >> 12) & 4));
    } else {
        v.omod = as_u8((w1 >> 27) & 3);
        if (vop3b) {
            v.carry = scalar(as_u8((w0 >> 8) & 0x7F), lanes);
        } else {
            v.abs = as_u8((w0 >> 8) & 7);
            v.opsel = as_u8((w0 >> 11) & 0xF);
        }
    }
    const std::uint32_t vdst = w0 & 0xFF;
    v.dst = compare ? scalar(as_u8(vdst), lanes) : valu_dst(name, vdst, dst_n);
    return v;
}

}  // namespace

bool ds_pair(std::string_view name) {
    for (const std::string_view token : split(name, '_'))
        for (const std::string_view family : kPairFamilies)
            if (token.starts_with(family)) return true;
    return false;
}

Valu Instruction::valu(std::string_view name, std::uint8_t lanes) const { return valu_field(*this, name, lanes); }

Fields Instruction::fields(bool wave64) const {
    const std::string_view name = this->name().value_or(std::string_view{});
    const auto w = [&](std::size_t index) { return index < words.size() ? words[index] : 0u; };
    const std::uint32_t w0 = w(0);
    const std::uint32_t w1 = w(1);
    const auto cache = [](bool glc, bool slc, bool dlc) { return Cache{glc, slc, dlc}; };
    switch (encoding) {
        case Encoding::Sop2:
        case Encoding::Sop1:
        case Encoding::Sopc: {
            const auto [dst_dw, src0_dw, src1_dw] = salu_dwords(name);
            const Operand sdst = scalar(as_u8((w0 >> 16) & 0x7F), dst_dw);
            const auto ssrc = [&](unsigned shift, std::uint8_t dwords) {
                return source((w0 >> shift) & 0xFF, dwords, w1);
            };
            SopFields sop;
            if (encoding == Encoding::Sop2) {
                sop.dst = sdst;
                sop.src = {ssrc(0, src0_dw), ssrc(8, src1_dw)};
            } else if (encoding == Encoding::Sopc) {
                sop.src = {ssrc(0, src0_dw), ssrc(8, src1_dw)};
            } else if (name == "s_getpc_b64") {
                sop.dst = sdst;
            } else if (name == "s_setpc_b64" || name == "s_rfe_b64") {
                sop.src = {ssrc(0, src0_dw)};
            } else {
                sop.dst = sdst;
                sop.src = {ssrc(0, src0_dw)};
            }
            return sop;
        }
        case Encoding::Sopk: {
            const Operand sdst = scalar(as_u8((w0 >> 16) & 0x7F),
                                        static_cast<std::uint8_t>(1 + (name == "s_call_b64" ? 1 : 0)));
            SopFields sop;
            if (name == "s_setreg_imm32_b32") {
                sop.src = {literal(w1)};
            } else if (name == "s_version") {
            } else if (name.starts_with("s_cmpk_") || name.starts_with("s_waitcnt_") || name == "s_setreg_b32") {
                sop.src = {sdst};
            } else {
                sop.dst = sdst;
            }
            sop.imm = static_cast<std::uint16_t>(w0);
            return sop;
        }
        case Encoding::Sopp: return SoppFields{static_cast<std::uint16_t>(w0)};
        case Encoding::Smem: {
            std::optional<std::uint8_t> data;
            if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb")
                data = std::nullopt;
            else if (name.starts_with("s_dcache_discard"))
                data = std::nullopt;
            else if (name == "s_memtime" || name == "s_memrealtime")
                data = std::uint8_t{2};
            else
                data = data_dwords(name);
            SmemFields smem;
            if (data) smem.data = scalar(as_u8((w0 >> 6) & 0x7F), *data);
            smem.base = scalar(as_u8((w0 & 0x3F) << 1),
                               static_cast<std::uint8_t>(2 + (name.starts_with("s_buffer_") ? 2 : 0)));
            smem.offset = sign_extend(w1 & 0x1FFFFF, 21);
            smem.soffset = scalar(as_u8(w1 >> 25), 1);
            smem.cache = cache(bit(w0, 16), false, bit(w0, 14));
            return smem;
        }
        case Encoding::Vop1:
        case Encoding::Vop2:
        case Encoding::Vopc:
        case Encoding::Vop3:
        case Encoding::Vop3p: return valu_field(*this, name, wave64 ? 2 : 1);
        case Encoding::Vintrp: {
            const std::uint32_t vsrc = w0 & 0xFF;
            VintrpFields vintrp;
            vintrp.dst = vector(w0 >> 18, 1);
            vintrp.src = name == "v_interp_mov_f32" ? constant(vsrc) : vector(vsrc, 1);
            vintrp.attr = attribute((w0 >> 10) & 0x3F, (w0 >> 8) & 3, false);
            return vintrp;
        }
        case Encoding::Ds: {
            const std::uint8_t ty = data_dwords(name);
            const bool pair = ds_pair(name);
            const bool returns = contains(name, "read") || contains(name, "_rtn") ||
                                 name == "ds_swizzle_b32" || name == "ds_permute_b32" || name == "ds_bpermute_b32" ||
                                 name == "ds_append" || name == "ds_consume" || name == "ds_ordered_count";
            const bool no_addr = name.starts_with("ds_gws") || contains(name, "addtid") || name == "ds_nop" ||
                                 name == "ds_append" || name == "ds_consume";
            std::uint8_t data_count = 1;
            if (contains(name, "read") || contains(name, "src2") || name == "ds_swizzle_b32" || name == "ds_nop" ||
                name == "ds_append" || name == "ds_consume" || name == "ds_ordered_count" ||
                name == "ds_gws_sema_v" || name == "ds_gws_sema_p" || name == "ds_gws_sema_release_all")
                data_count = 0;
            else if (pair || contains(name, "cmpst") || contains(name, "mskor"))
                data_count = 2;
            DsFields ds;
            if (returns) ds.dst = vector(w1 >> 24, static_cast<std::uint8_t>(ty * (pair ? 2 : 1)));
            if (!no_addr) ds.addr = vector(w1 & 0xFF, 1);
            for (std::uint8_t i = 0; i < data_count; ++i)
                ds.data.push_back(vector((w1 >> (8 + 8 * i)) & 0xFF, ty));
            ds.offset0 = as_u8(w0);
            ds.offset1 = as_u8(w0 >> 8);
            ds.gds = bit(w0, 17);
            return ds;
        }
        case Encoding::Mubuf:
        case Encoding::Mtbuf: {
            const bool offen = bit(w0, 12);
            const bool idxen = bit(w0, 13);
            const bool tfe = bit(w1, 23);
            const bool mtbuf = encoding == Encoding::Mtbuf;
            BufferFields buffer;
            if (!name.ends_with("_inv"))
                buffer.data = vector((w1 >> 8) & 0xFF,
                                     static_cast<std::uint8_t>(data_dwords(name) + (tfe ? 1 : 0)));
            buffer.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + (offen && idxen ? 1 : 0)));
            buffer.rsrc = scalar(as_u8(((w1 >> 16) & 0x1F) << 2), 4);
            buffer.soffset = source(w1 >> 24, 1, 0);
            buffer.offset = static_cast<std::uint16_t>(w0 & 0xFFF);
            buffer.offen = offen;
            buffer.idxen = idxen;
            if (mtbuf) buffer.format = as_u8((w0 >> 19) & 0x7F);
            buffer.lds = !mtbuf && bit(w0, 16);
            buffer.tfe = tfe;
            buffer.cache = cache(bit(w0, 14), bit(w1, 22), bit(w0, 15));
            return buffer;
        }
        case Encoding::Mimg: {
            const std::uint8_t dim = as_u8((w0 >> 3) & 7);
            const std::uint8_t dmask = as_u8((w0 >> 8) & 0xF);
            const bool a16 = bit(w1, 30);
            const bool d16 = bit(w1, 31);
            const bool r128 = bit(w0, 15);
            const bool tfe = bit(w0, 16);
            const bool lwe = bit(w0, 17);
            std::uint8_t data_count =
                name.starts_with("image_gather4") ? 4 : static_cast<std::uint8_t>(std::max(std::popcount(dmask), 1));
            if (d16) data_count = static_cast<std::uint8_t>((data_count + 1) / 2);
            const std::uint8_t count = image_addr_dwords(name, dim, a16);
            ImageFields image;
            if (((w0 >> 1) & 3) == 0) {
                image.addr.push_back(vector(w1 & 0xFF, count));
            } else {
                for (std::uint8_t i = 0; i < count; ++i) {
                    if (i == 0)
                        image.addr.push_back(vector(w1 & 0xFF, 1));
                    else
                        image.addr.push_back(vector((w(2 + (i - 1) / 4) >> (8 * ((i - 1) % 4))) & 0xFF, 1));
                }
            }
            const bool sampled = name.starts_with("image_sample") || name.starts_with("image_gather4") ||
                                 name.starts_with("image_get_lod");
            image.data = vector((w1 >> 8) & 0xFF, static_cast<std::uint8_t>(data_count + (tfe || lwe ? 1 : 0)));
            image.rsrc = scalar(as_u8(((w1 >> 16) & 0x1F) << 2), static_cast<std::uint8_t>(r128 ? 4 : 8));
            if (sampled) image.sampler = scalar(as_u8(((w1 >> 21) & 0x1F) << 2), 4);
            image.dmask = dmask;
            image.dim = dim;
            image.unorm = bit(w0, 12);
            image.r128 = r128;
            image.a16 = a16;
            image.d16 = d16;
            image.tfe = tfe;
            image.lwe = lwe;
            image.cache = cache(bit(w0, 13), bit(w0, 25), bit(w0, 7));
            return image;
        }
        case Encoding::Flat:
        case Encoding::Global:
        case Encoding::Scratch: {
            const std::uint8_t saddr = as_u8((w1 >> 16) & 0x7F);
            const bool glc = bit(w0, 16);
            const std::uint8_t n = data_dwords(name);
            const bool load = contains(name, "_load_");
            const bool atomic = contains(name, "_atomic_");
            FlatFields flat;
            if (encoding == Encoding::Flat)
                flat.addr = vector(w1 & 0xFF, 2);
            else if (encoding == Encoding::Global)
                flat.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + (saddr == kNull ? 1 : 0)));
            else if (saddr == kNull)
                flat.addr = vector(w1 & 0xFF, 1);
            const std::uint8_t ret = atomic && contains(name, "cmpswap") ? static_cast<std::uint8_t>(n / 2) : n;
            if (load || (atomic && glc)) flat.dst = vector(w1 >> 24, ret);
            if (!load) flat.data = vector((w1 >> 8) & 0xFF, n);
            if (encoding != Encoding::Flat)
                flat.saddr = scalar(saddr, static_cast<std::uint8_t>(1 + (encoding == Encoding::Global ? 1 : 0)));
            flat.offset = encoding == Encoding::Flat ? static_cast<std::int16_t>(w0 & 0x7FF)
                                                     : static_cast<std::int16_t>(sign_extend(w0 & 0xFFF, 12));
            flat.lds = bit(w0, 13);
            flat.cache = cache(glc, bit(w0, 17), bit(w0, 12));
            return flat;
        }
        case Encoding::Exp: {
            const bool compr = bit(w0, 10);
            ExpFields exp;
            exp.target = as_u8((w0 >> 4) & 0x3F);
            for (std::uint32_t i = 0; i < 4; ++i) {
                const std::uint32_t slot = compr ? i / 2 : i;
                if (bit(w0, i)) exp.src[i] = vector((w1 >> (8 * slot)) & 0xFF, 1);
            }
            exp.compr = compr;
            exp.done = bit(w0, 11);
            exp.vm = bit(w0, 12);
            return exp;
        }
    }
    return {};
}

}  // namespace g6shim::shader
