// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/cpu_emulation.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

namespace g6shim::cpu {

namespace {

constexpr std::size_t kPageBytes = 0x1000;
constexpr std::uint8_t kTwoByteOpcode = 0x0f;
constexpr std::uint8_t kSystemOpcode = 0x01;
constexpr std::uint8_t kClzeroOpcode = 0xfc;
constexpr std::uint8_t kMonitorxOpcode = 0xfa;
constexpr std::uint8_t kMwaitxOpcode = 0xfb;
constexpr std::uint8_t kOperandSizePrefix = 0x66;
constexpr std::uint8_t kCacheLineOpcode = 0xae;
constexpr std::uint8_t kWriteBackExtension = 6;
constexpr std::uint8_t kModRmRegisterMode = 3;
constexpr std::uint8_t kAddOpcode = 0x83;
constexpr std::uint8_t kAddImmediate = 0x40;
constexpr std::uint8_t kCompareOpcode = 0x39;
constexpr std::uint8_t kJumpBelowOrEqual = 0x76;
constexpr std::uint8_t kSse4aImmediateOpcode = 0x78;
constexpr std::uint8_t kSse4aRegisterOpcode = 0x79;
constexpr std::uint8_t kInsertqPrefix = 0xf2;
constexpr std::uint8_t kRexPrefixBase = 0x40;
constexpr std::uint8_t kRexPrefixMask = 0xf0;
constexpr std::uint8_t kRexWriteBit = 0x08;
constexpr std::uint8_t kRexBaseBit = 0x01;
constexpr std::uint8_t kRexRegisterBit = 0x04;
constexpr std::uint32_t kSse4aFieldMask = 63;
constexpr std::uint32_t kSse4aFullWidth = 64;

enum class Emulated { Clzero, Monitorx, Mwaitx, Clwb, Extrq, Insertq };

constexpr std::size_t kEmulatedKinds = 6;

std::atomic<bool> g_reported[kEmulatedKinds];

const char* emulated_name(Emulated kind) {
    switch (kind) {
        case Emulated::Clzero: return "clzero";
        case Emulated::Monitorx: return "monitorx";
        case Emulated::Mwaitx: return "mwaitx";
        case Emulated::Clwb: return "clwb";
        case Emulated::Extrq: return "extrq";
        default: return "insertq";
    }
}

void report_first(Emulated kind) {
    std::atomic<bool>& reported = g_reported[static_cast<std::size_t>(kind)];
    if (!reported.exchange(true, std::memory_order_relaxed)) G6SHIM_INFO("emulating %s", emulated_name(kind));
}

bool has_rex_prefix(std::uint8_t byte) { return (byte & kRexPrefixMask) == kRexPrefixBase; }

std::uint64_t& register_numbered(CONTEXT& context, std::uint8_t number) {
    switch (number & 0xf) {
        case 0: return context.Rax;
        case 1: return context.Rcx;
        case 2: return context.Rdx;
        case 3: return context.Rbx;
        case 4: return context.Rsp;
        case 5: return context.Rbp;
        case 6: return context.Rsi;
        case 7: return context.Rdi;
        case 8: return context.R8;
        case 9: return context.R9;
        case 10: return context.R10;
        case 11: return context.R11;
        case 12: return context.R12;
        case 13: return context.R13;
        case 14: return context.R14;
        default: return context.R15;
    }
}

struct Sse4aResult {
    std::size_t length;
    bool extract;
};

std::optional<Sse4aResult> sse4a(const std::uint8_t* code, std::size_t size, M128A (&xmm)[16]) {
    if (size < 2) return std::nullopt;
    const std::uint8_t prefix = code[0];
    const bool rex = has_rex_prefix(code[1]);
    const std::size_t at = rex ? 2 : 1;
    if (prefix != kOperandSizePrefix && prefix != kInsertqPrefix) return std::nullopt;
    if (at + 3 > size) return std::nullopt;
    if (code[at] != kTwoByteOpcode) return std::nullopt;
    if (code[at + 1] != kSse4aImmediateOpcode && code[at + 1] != kSse4aRegisterOpcode) return std::nullopt;
    const std::uint8_t modrm = code[at + 2];
    if ((modrm >> 6) != kModRmRegisterMode) return std::nullopt;

    const std::uint8_t rex_byte = rex ? code[1] : 0;
    const std::size_t dst = ((modrm >> 3) & 7) | ((rex_byte & kRexRegisterBit) << 1);
    const std::size_t src = (modrm & 7) | ((rex_byte & kRexBaseBit) << 3);
    const bool immediate = code[at + 1] == kSse4aImmediateOpcode;

    std::uint8_t length_field;
    std::uint8_t index_field;
    std::size_t length;
    if (immediate) {
        if (at + 5 > size) return std::nullopt;
        length_field = code[at + 3];
        index_field = code[at + 4];
        length = at + 5;
    } else {
        const std::uint64_t fields =
            prefix == kOperandSizePrefix ? xmm[src].Low : static_cast<std::uint64_t>(xmm[src].High);
        length_field = static_cast<std::uint8_t>(fields);
        index_field = static_cast<std::uint8_t>(fields >> 8);
        length = at + 3;
    }

    const std::uint32_t field_bits = length_field & kSse4aFieldMask;
    const std::uint32_t bits = field_bits == 0 ? kSse4aFullWidth : field_bits;
    const std::uint32_t offset = index_field & kSse4aFieldMask;
    const std::uint64_t mask = bits == kSse4aFullWidth ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;

    if (prefix == kOperandSizePrefix) {
        if (immediate && ((modrm >> 3) & 7) != 0) return std::nullopt;
        const std::size_t target = immediate ? src : dst;
        xmm[target].Low = (xmm[target].Low >> offset) & mask;
        return Sse4aResult{length, true};
    }

    const std::uint64_t value = (xmm[src].Low & mask) << offset;
    xmm[dst].Low = (xmm[dst].Low & ~(mask << offset)) | value;
    return Sse4aResult{length, false};
}

LONG NTAPI exception_handler(EXCEPTION_POINTERS* exception) {
    return emulate_amd_only(exception) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

std::atomic<bool> g_installed{false};

}  // namespace

std::optional<ClwbLoop> clwb_loop(const std::uint8_t* code, std::size_t size) {
    if (size < 2) return std::nullopt;
    const bool rex = code[1] == 0x40 || code[1] == 0x41;
    const std::uint8_t rex_byte = rex ? code[1] : 0;
    const std::size_t at = rex ? 2 : 1;
    if (code[0] != kOperandSizePrefix) return std::nullopt;
    if (at + 3 > size) return std::nullopt;
    if (code[at] != kTwoByteOpcode || code[at + 1] != kCacheLineOpcode) return std::nullopt;

    const std::uint8_t modrm = code[at + 2];
    const std::uint8_t rm = modrm & 7;
    const std::uint8_t mode = modrm >> 6;
    const std::uint8_t reg = (modrm >> 3) & 7;
    if (mode != 0 || reg != kWriteBackExtension || rm == 4 || rm == 5) return std::nullopt;

    const std::uint8_t base = static_cast<std::uint8_t>(rm | ((rex_byte & kRexBaseBit) << 3));
    const std::size_t add_at = at + 3;
    if (add_at + 4 > size) return std::nullopt;
    if (code[add_at] != (kRexPrefixBase | kRexWriteBit | (base >> 3))) return std::nullopt;
    if (code[add_at + 1] != kAddOpcode) return std::nullopt;
    if (code[add_at + 2] != (0xc0 | (base & 7))) return std::nullopt;
    if (code[add_at + 3] != kAddImmediate) return std::nullopt;

    const std::size_t compare_at = add_at + 4;
    if (compare_at + 3 > size) return std::nullopt;
    const std::uint8_t compare_rex = code[compare_at];
    if (compare_rex < (kRexPrefixBase | kRexWriteBit) || compare_rex > (kRexPrefixBase | kRexWriteBit | 7)) {
        return std::nullopt;
    }
    if (code[compare_at + 1] != kCompareOpcode) return std::nullopt;
    const std::uint8_t compare_modrm = code[compare_at + 2];
    if ((compare_modrm >> 6) != kModRmRegisterMode) return std::nullopt;
    if (((compare_modrm & 7) | ((compare_rex & kRexBaseBit) << 3)) != base) return std::nullopt;

    const std::uint8_t limit =
        static_cast<std::uint8_t>(((compare_modrm >> 3) & 7) | ((compare_rex & kRexRegisterBit) << 1));
    const std::size_t jump_at = compare_at + 3;
    if (jump_at + 2 > size) return std::nullopt;
    const std::size_t length = jump_at + 2;
    if (code[jump_at] != kJumpBelowOrEqual) return std::nullopt;
    if (static_cast<std::int8_t>(code[jump_at + 1]) != -static_cast<std::int8_t>(length)) return std::nullopt;
    if (limit == base) return std::nullopt;

    return ClwbLoop{base, limit, length};
}

std::optional<std::size_t> clwb_length(const std::array<std::uint8_t, kMaxInstructionLength>& code) {
    const std::size_t at = (code[1] & kRexPrefixMask) == kRexPrefixBase ? 2 : 1;
    if (code[0] != kOperandSizePrefix || code[at] != kTwoByteOpcode || code[at + 1] != kCacheLineOpcode) {
        return std::nullopt;
    }
    const std::uint8_t modrm = code[at + 2];
    const std::uint8_t mode = modrm >> 6;
    const std::uint8_t rm = modrm & 7;
    if (mode == kModRmRegisterMode || ((modrm >> 3) & 7) != kWriteBackExtension) return std::nullopt;

    std::size_t length = at + 3;
    std::uint8_t base = rm;
    if (rm == 4) {
        base = code[length] & 7;
        ++length;
    }
    if (mode == 0) {
        length += rm == 5 || (rm == 4 && base == 5) ? 4 : 0;
    } else if (mode == 1) {
        length += 1;
    } else {
        length += 4;
    }
    return length;
}

std::uint32_t compare_flags(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t result = a - b;
    const std::uint32_t carry = static_cast<std::uint32_t>(a < b);
    const std::uint32_t parity =
        static_cast<std::uint32_t>(std::popcount(static_cast<std::uint8_t>(result)) % 2 == 0);
    const std::uint32_t adjust = static_cast<std::uint32_t>(((a ^ b ^ result) >> 4) & 1);
    const std::uint32_t zero = static_cast<std::uint32_t>(result == 0);
    const std::uint32_t sign = static_cast<std::uint32_t>((result >> 63) & 1);
    const std::uint32_t overflow = static_cast<std::uint32_t>((((a ^ b) & (a ^ result)) >> 63) & 1);
    return carry | parity << 2 | adjust << 4 | zero << 6 | sign << 7 | overflow << 11;
}

bool emulate_amd_only(EXCEPTION_POINTERS* exception) {
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr) {
        return false;
    }
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return false;

    CONTEXT& context = *exception->ContextRecord;
    const auto* code = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(context.Rip));
    const std::size_t room =
        kPageBytes - (reinterpret_cast<std::uintptr_t>(code) & (kPageBytes - 1));
    const std::size_t size = std::min(room, kInstructionReadBytes);

    std::uint8_t bytes[kInstructionReadBytes] = {};
    std::memcpy(bytes, code, size);

    if (size >= 3 && bytes[0] == kTwoByteOpcode && bytes[1] == kSystemOpcode) {
        if (bytes[2] == kClzeroOpcode) {
            const std::uint64_t line = context.Rax & ~(static_cast<std::uint64_t>(kCacheLineBytes) - 1);
            std::memset(reinterpret_cast<void*>(line), 0, kCacheLineBytes);
            context.Rip += 3;
            report_first(Emulated::Clzero);
            return true;
        }
        if (bytes[2] == kMonitorxOpcode) {
            context.Rip += 3;
            report_first(Emulated::Monitorx);
            return true;
        }
        if (bytes[2] == kMwaitxOpcode) {
            SwitchToThread();
            context.Rip += 3;
            report_first(Emulated::Mwaitx);
            return true;
        }
    }

    if (auto loop = clwb_loop(bytes, size)) {
        const std::uint64_t limit = register_numbered(context, loop->limit);
        std::uint64_t& base = register_numbered(context, loop->base);
        if (base <= limit) {
            base += ((limit - base) / kCacheLineBytes + 1) * kCacheLineBytes;
        } else {
            base += kCacheLineBytes;
        }
        const std::uint32_t flags = compare_flags(base, limit);
        context.EFlags = (context.EFlags & ~kComparedFlags) | flags;
        context.Rip += loop->length;
        report_first(Emulated::Clwb);
        return true;
    }

    if (size >= kMaxInstructionLength) {
        std::array<std::uint8_t, kMaxInstructionLength> head{};
        std::memcpy(head.data(), bytes, head.size());
        if (auto length = clwb_length(head)) {
            context.Rip += *length;
            report_first(Emulated::Clwb);
            return true;
        }
    }

    if (auto result = sse4a(bytes, size, context.FltSave.XmmRegisters)) {
        context.Rip += result->length;
        report_first(result->extract ? Emulated::Extrq : Emulated::Insertq);
        return true;
    }
    return false;
}

void install_amd_emulation() {
    if (g_installed.exchange(true, std::memory_order_relaxed)) return;
    AddVectoredExceptionHandler(1, exception_handler);
}

}  // namespace g6shim::cpu

#if defined(G6SHIM_RUNTIME_BUILD)
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g6shim::cpu::install_amd_emulation();
    return TRUE;
}
#endif
