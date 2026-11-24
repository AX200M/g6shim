// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/gfx10.hpp"

#include <algorithm>
#include <cstddef>
#include <map>

#include "g6shim/shader/error.hpp"
#include "gfx10_opcodes.hpp"

namespace g6shim::shader {

namespace {

using namespace opcodes;

std::span<const OpcodeEntry> table_for(Encoding encoding) {
    switch (encoding) {
        case Encoding::Sop2: return kSop2;
        case Encoding::Sopk: return kSopk;
        case Encoding::Sop1: return kSop1;
        case Encoding::Sopc: return kSopc;
        case Encoding::Sopp: return kSopp;
        case Encoding::Smem: return kSmem;
        case Encoding::Vop2: return kVop2;
        case Encoding::Vop1: return kVop1;
        case Encoding::Vopc: return kVopc;
        case Encoding::Vop3: return kVop3;
        case Encoding::Vop3p: return kVop3p;
        case Encoding::Vintrp: return kVintrp;
        case Encoding::Ds: return kDs;
        case Encoding::Mubuf: return kMubuf;
        case Encoding::Mtbuf: return kMtbuf;
        case Encoding::Mimg: return kMimg;
        case Encoding::Flat: return kFlat;
        case Encoding::Global: return kGlobal;
        case Encoding::Scratch: return kScratch;
        case Encoding::Exp: break;
    }
    return {};
}

std::optional<std::string_view> lookup(std::span<const OpcodeEntry> table, std::uint16_t code) {
    const auto it = std::lower_bound(table.begin(), table.end(), code,
                                     [](const OpcodeEntry& entry, std::uint16_t key) { return entry.code < key; });
    if (it == table.end() || it->code != code) return std::nullopt;
    return std::string_view{it->name};
}

using Found = std::map<std::size_t, Instruction>;

const Instruction* previous(const Found& found, std::size_t at) {
    auto it = found.lower_bound(at);
    if (it == found.begin()) return nullptr;
    --it;
    return &it->second;
}

std::string_view name_of(const Instruction& inst) {
    const auto name = inst.name();
    return name ? *name : std::string_view{};
}

std::size_t branch_target(const Instruction& inst, std::size_t next, std::size_t len) {
    const auto simm = static_cast<std::int16_t>(static_cast<std::uint16_t>(inst.words[0]));
    const std::int64_t target = static_cast<std::int64_t>(next) + 4 * static_cast<std::int64_t>(simm);
    if (target < 0 || static_cast<std::uint64_t>(target) >= len) detail::fail_isa(inst.offset, "branch target outside the code");
    return static_cast<std::size_t>(target);
}

Control flow(const Instruction& inst, std::size_t next, std::size_t len) {
    const std::string_view name = name_of(inst);
    if (inst.encoding == Encoding::Sopp) {
        if (name == "s_endpgm" || name == "s_endpgm_saved" || name == "s_endpgm_ordered_ps_done" || name == "s_code_end")
            return {Control::Kind::Stop, 0};
        if (name == "s_branch") return {Control::Kind::Jump, branch_target(inst, next, len)};
        if (name.starts_with("s_cbranch_")) return {Control::Kind::Branch, branch_target(inst, next, len)};
    } else if (inst.encoding == Encoding::Sopk) {
        if (name == "s_call_b64") return {Control::Kind::Branch, branch_target(inst, next, len)};
    } else if (inst.encoding == Encoding::Sop1) {
        if (name == "s_setpc_b64") return {Control::Kind::Stop, 0};
    }
    return {Control::Kind::Next, 0};
}

template <typename Previous>
std::optional<std::size_t> far_jump(Previous previous, const Instruction& setpc, std::size_t len) {
    const std::uint32_t n = setpc.words[0] & 0xFF;
    std::optional<std::int64_t> delta;
    std::size_t at = setpc.offset;
    for (int step = 0; step < 4; ++step) {
        const Instruction* prev = previous(at);
        if (prev == nullptr) break;
        if (prev->offset + 4 * prev->words.size() != at) break;
        const std::uint32_t w = prev->words[0];
        const std::uint32_t sdst = (w >> 16) & 0x7F;
        const std::uint32_t ssrc0 = w & 0xFF;
        const std::uint32_t ssrc1 = (w >> 8) & 0xFF;
        const std::string_view name = name_of(*prev);
        if (prev->encoding == Encoding::Sop2 && (name == "s_add_u32" || name == "s_sub_u32") && sdst == n &&
            ssrc0 == n && ssrc1 == 0xFF && !delta && prev->words.size() > 1) {
            const auto literal = static_cast<std::int64_t>(static_cast<std::int32_t>(prev->words[1]));
            delta = name == "s_add_u32" ? literal : -literal;
        } else if (prev->encoding == Encoding::Sop1 && name == "s_getpc_b64" && sdst == n) {
            if (!delta) break;
            const std::int64_t target = static_cast<std::int64_t>(prev->offset) + 4 + *delta;
            if (target < 0 || static_cast<std::uint64_t>(target) >= len)
                detail::fail_isa(setpc.offset, "far jump target outside the code");
            return static_cast<std::size_t>(target);
        }
        at = prev->offset;
    }
    return std::nullopt;
}

template <typename Previous>
Control control_from(const Instruction& inst, std::size_t len, Previous previous) {
    const std::size_t next = inst.offset + 4 * inst.words.size();
    const Control result = flow(inst, next, len);
    if (result.kind == Control::Kind::Stop && name_of(inst) == "s_setpc_b64") {
        const auto target = far_jump(previous, inst, len);
        return target ? Control{Control::Kind::Jump, *target} : Control{Control::Kind::Stop, 0};
    }
    return result;
}

bool has_literal(std::uint32_t src) {
    return src == 0xFF || src == 0xF9 || src == 0xFA || src == 0xE9 || src == 0xEA;
}

bool is_madmk_or_madak(Encoding encoding, std::uint16_t opcode) {
    const auto name = opcode_name(encoding, opcode);
    if (!name) return false;
    const std::string_view n = *name;
    return n == "v_madmk_f32" || n == "v_madak_f32" || n == "v_fmamk_f32" || n == "v_fmaak_f32" ||
           n == "v_madmk_f16" || n == "v_madak_f16" || n == "v_fmamk_f16" || n == "v_fmaak_f16";
}

}  // namespace

std::span<const OpcodeEntry> opcode_table(Encoding encoding) { return table_for(encoding); }

std::optional<std::string_view> opcode_name(Encoding encoding, std::uint16_t opcode) {
    switch (encoding) {
        case Encoding::Exp: return std::string_view{"exp"};
        case Encoding::Vop3:
            if (opcode < 0x100) return lookup(table_for(Encoding::Vopc), opcode);
            if (opcode < 0x140) return lookup(table_for(Encoding::Vop2), static_cast<std::uint16_t>(opcode - 0x100));
            if (opcode >= 0x180 && opcode < 0x200)
                return lookup(table_for(Encoding::Vop1), static_cast<std::uint16_t>(opcode - 0x180));
            return lookup(table_for(Encoding::Vop3), opcode);
        default: break;
    }
    return lookup(table_for(encoding), opcode);
}

std::optional<std::string_view> Instruction::name() const { return opcode_name(encoding, opcode); }

namespace {

Instruction decode_instruction(std::span<const std::uint8_t> code, std::size_t offset) {
    const auto word = [&](std::size_t index) {
        const std::size_t at = offset + 4 * index;
        if (at > code.size() || code.size() - at < 4) detail::fail_isa(offset, "instruction runs past the end of the code");
        return static_cast<std::uint32_t>(code[at]) | static_cast<std::uint32_t>(code[at + 1]) << 8 |
               static_cast<std::uint32_t>(code[at + 2]) << 16 | static_cast<std::uint32_t>(code[at + 3]) << 24;
    };
    const std::uint32_t w = word(0);
    Encoding encoding = Encoding::Vop2;
    std::uint16_t opcode = 0;
    std::size_t len = 1;
    if (w >> 31 == 0) {
        const std::uint32_t src0 = w & 0x1FF;
        const std::size_t extra = has_literal(src0) ? 1 : 0;
        switch (w >> 25) {
            case 0x3F:
                encoding = Encoding::Vop1;
                opcode = static_cast<std::uint16_t>((w >> 9) & 0xFF);
                len = 1 + extra;
                break;
            case 0x3E:
                encoding = Encoding::Vopc;
                opcode = static_cast<std::uint16_t>((w >> 17) & 0xFF);
                len = 1 + extra;
                break;
            default:
                encoding = Encoding::Vop2;
                opcode = static_cast<std::uint16_t>(w >> 25);
                len = 1 + std::max(extra, static_cast<std::size_t>(is_madmk_or_madak(Encoding::Vop2, opcode) ? 1 : 0));
                break;
        }
    } else if (w >> 30 == 0b10) {
        const auto literal = [](std::uint32_t a, std::uint32_t b) {
            return 1 + static_cast<std::size_t>(a == 0xFF || b == 0xFF ? 1 : 0);
        };
        switch (w >> 23) {
            case 0x17D:
                encoding = Encoding::Sop1;
                opcode = static_cast<std::uint16_t>((w >> 8) & 0xFF);
                len = literal(w & 0xFF, 0u);
                break;
            case 0x17E:
                encoding = Encoding::Sopc;
                opcode = static_cast<std::uint16_t>((w >> 16) & 0x7F);
                len = literal(w & 0xFF, (w >> 8) & 0xFF);
                break;
            case 0x17F:
                encoding = Encoding::Sopp;
                opcode = static_cast<std::uint16_t>((w >> 16) & 0x7F);
                len = 1;
                break;
            default:
                if (w >> 28 == 0b1011) {
                    encoding = Encoding::Sopk;
                    opcode = static_cast<std::uint16_t>((w >> 23) & 0x1F);
                    len = 1 + static_cast<std::size_t>(
                                  opcode_name(Encoding::Sopk, opcode) == std::string_view{"s_setreg_imm32_b32"} ? 1 : 0);
                } else {
                    encoding = Encoding::Sop2;
                    opcode = static_cast<std::uint16_t>((w >> 23) & 0x7F);
                    len = literal(w & 0xFF, (w >> 8) & 0xFF);
                }
                break;
        }
    } else {
        const auto vop3_len = [](std::uint32_t w1) {
            const bool literal = (w1 & 0x1FF) == 0xFF || ((w1 >> 9) & 0x1FF) == 0xFF || ((w1 >> 18) & 0x1FF) == 0xFF;
            return 2 + static_cast<std::size_t>(literal ? 1 : 0);
        };
        switch (w >> 26) {
            case 0x32:
                encoding = Encoding::Vintrp;
                opcode = static_cast<std::uint16_t>((w >> 16) & 0x3);
                len = 1;
                break;
            case 0x33:
                encoding = Encoding::Vop3p;
                opcode = static_cast<std::uint16_t>((w >> 16) & 0x7F);
                len = vop3_len(word(1));
                break;
            case 0x35:
                encoding = Encoding::Vop3;
                opcode = static_cast<std::uint16_t>((w >> 16) & 0x3FF);
                len = vop3_len(word(1));
                break;
            case 0x36:
                encoding = Encoding::Ds;
                opcode = static_cast<std::uint16_t>((w >> 18) & 0xFF);
                len = 2;
                break;
            case 0x37:
                switch ((w >> 14) & 3) {
                    case 0: encoding = Encoding::Flat; break;
                    case 1: encoding = Encoding::Scratch; break;
                    case 2: encoding = Encoding::Global; break;
                    default: detail::fail_isa(offset, "reserved FLAT segment");
                }
                opcode = static_cast<std::uint16_t>((w >> 18) & 0x7F);
                len = 2;
                break;
            case 0x38:
                encoding = Encoding::Mubuf;
                opcode = static_cast<std::uint16_t>(((w >> 18) & 0x7F) | ((w >> 25) & 1) << 7);
                len = 2;
                break;
            case 0x3A:
                encoding = Encoding::Mtbuf;
                opcode = static_cast<std::uint16_t>(((w >> 16) & 0x7) | ((word(1) >> 21) & 1) << 3);
                len = 2;
                break;
            case 0x3C:
                encoding = Encoding::Mimg;
                opcode = static_cast<std::uint16_t>(((w >> 18) & 0x7F) | (w & 1) << 7);
                len = 2 + static_cast<std::size_t>((w >> 1) & 3);
                break;
            case 0x3D:
                encoding = Encoding::Smem;
                opcode = static_cast<std::uint16_t>((w >> 18) & 0xFF);
                len = 2;
                break;
            case 0x3E:
                encoding = Encoding::Exp;
                opcode = 0;
                len = 2;
                break;
            default: detail::fail_isa(offset, "unknown encoding");
        }
    }
    Instruction inst;
    inst.offset = offset;
    inst.encoding = encoding;
    inst.opcode = opcode;
    inst.words.reserve(len);
    for (std::size_t i = 0; i < len; ++i) inst.words.push_back(word(i));
    return inst;
}

}  // namespace

std::vector<Instruction> decode(std::span<const std::uint8_t> code) {
    if (code.size() % 4 != 0) detail::fail_isa(code.size(), "code size is not a multiple of 4");
    Found found;
    std::vector<std::size_t> pending{0};
    while (!pending.empty()) {
        std::size_t offset = pending.back();
        pending.pop_back();
        while (found.find(offset) == found.end()) {
            Instruction inst = decode_instruction(code, offset);
            const std::size_t next = offset + 4 * inst.words.size();
            const Control result = control_from(inst, code.size(), [&](std::size_t at) { return previous(found, at); });
            found.emplace(offset, std::move(inst));
            bool stop = false;
            switch (result.kind) {
                case Control::Kind::Next: offset = next; break;
                case Control::Kind::Stop: stop = true; break;
                case Control::Kind::Jump: offset = result.target; break;
                case Control::Kind::Branch:
                    pending.push_back(result.target);
                    offset = next;
                    break;
            }
            if (stop) break;
        }
    }
    std::vector<Instruction> list;
    list.reserve(found.size());
    for (auto& [at, inst] : found) list.push_back(std::move(inst));
    for (std::size_t i = 1; i < list.size(); ++i) {
        const Instruction& prev = list[i - 1];
        if (prev.offset + 4 * prev.words.size() > list[i].offset)
            detail::fail_isa(list[i].offset, "a branch lands inside an instruction");
    }
    return list;
}

Control control(const Instruction& inst, std::size_t code_len, std::span<const Instruction> preceding) {
    const auto previous_of = [&](std::size_t at) -> const Instruction* {
        const auto it = std::lower_bound(preceding.begin(), preceding.end(), at,
                                         [](const Instruction& entry, std::size_t key) { return entry.offset < key; });
        return it == preceding.begin() ? nullptr : &*(it - 1);
    };
    return control_from(inst, code_len, previous_of);
}

}  // namespace g6shim::shader
