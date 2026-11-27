// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lower_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace g6shim::shader::emit {

namespace {

constexpr std::uint32_t kRawBuffer = 11;
constexpr std::uint32_t kUnorderedView = 0x1000;
constexpr std::uint32_t kHighAddressMask = 0xFFFF;
constexpr std::uint32_t kStrideShift = 16;
constexpr std::uint32_t kStrideMask = 0x3FFF;
constexpr std::uint32_t kMaxLoadDwords = 4;
constexpr std::uint32_t kDwordBytes = 4;
constexpr std::uint32_t kSignBit = 0x8000'0000u;
constexpr std::uint32_t kAtomicSignedMin = 4;
constexpr std::uint32_t kAtomicSignedMax = 5;
constexpr std::uint32_t kAtomicUnsignedMin = 6;
constexpr std::uint32_t kAtomicUnsignedMax = 7;

std::optional<std::uint32_t> dwords_named(std::string_view rest) {
    if (rest == "dword") return 1;
    if (rest == "dwordx2") return 2;
    if (rest == "dwordx3") return 3;
    if (rest == "dwordx4") return 4;
    if (rest == "dwordx8") return 8;
    if (rest == "dwordx16") return 16;
    return std::nullopt;
}

std::optional<std::uint32_t> atomic_code(std::string_view op) {
    if (op == "add") return 0;
    if (op == "and") return 1;
    if (op == "or") return 2;
    if (op == "xor") return 3;
    if (op == "smin") return 4;
    if (op == "smax") return 5;
    if (op == "umin") return 6;
    if (op == "umax") return 7;
    if (op == "swap") return 8;
    if (op == "cmpswap") return UINT32_MAX;
    return std::nullopt;
}

}  // namespace

std::vector<V> Lower::flat_table_load(V byte, std::uint32_t count) {
    const std::size_t c = cur_;
    const V table = s_.flat_table_handle();
    const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << count) - 1u));
    const V r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                      {table, byte, s_.m.undef(s_.t.i32), mask, s_.m.i32(kDwordBytes)});
    std::vector<V> out;
    for (std::uint32_t k = 0; k < count; ++k) out.push_back(as_int(s_.m.extract(c, r, k)));
    return out;
}

V Lower::at_or_below(V a_low, V a_high, V b_low, V b_high) {
    const std::size_t c = cur_;
    const V high_below = s_.m.cmp(c, pred::kIult, a_high, b_high);
    const V high_same = s_.m.cmp(c, pred::kEq, a_high, b_high);
    const V low_at_or_below = s_.m.cmp(c, pred::kIule, a_low, b_low);
    return s_.m.bin(c, Bin::Or, high_below, s_.m.bin(c, Bin::And, high_same, low_at_or_below));
}

std::pair<V, V> Lower::add_address(V low, V high, V bytes) {
    const std::size_t c = cur_;
    const auto [sum, carry] = add_sub_carry(c, true, low, bytes, std::nullopt);
    const V up = s_.m.cast(c, Cast::ZExt, carry, s_.t.i32);
    return {sum, s_.m.bin(c, Bin::Add, high, up)};
}

FlatTarget Lower::flat_lookup(V low, V high, std::optional<V> in_range) {
    const std::size_t c = cur_;
    const std::vector<V> header = flat_table_load(s_.m.i32(0), 2);
    const V count = header[0];
    const V null_index = header[1];
    const V entry_bytes = s_.m.i32(kFlatEntryBytes);
    const V header_bytes = s_.m.i32(kFlatHeaderBytes);
    const auto entry_at = [&](V index) {
        return s_.m.bin(c, Bin::Add, s_.m.bin(c, Bin::Mul, index, entry_bytes), header_bytes);
    };
    V position = s_.m.i32(0);
    for (std::uint32_t step = 1u << (kFlatSearchSteps - 1); step != 0; step >>= 1) {
        const V candidate = s_.m.bin(c, Bin::Add, position, s_.m.i32(step));
        const V within = s_.m.cmp(c, pred::kIult, candidate, count);
        const std::vector<V> base = flat_table_load(entry_at(candidate), 2);
        const V below = at_or_below(base[0], base[1], low, high);
        position = s_.m.select(c, s_.m.bin(c, Bin::And, within, below), candidate, position);
    }
    const std::vector<V> entry = flat_table_load(entry_at(position), 4);
    const auto [offset, borrow] = add_sub_carry(c, false, low, entry[0], std::nullopt);
    V offset_high = s_.m.bin(c, Bin::Sub, high, entry[1]);
    offset_high = s_.m.bin(c, Bin::Sub, offset_high, s_.m.cast(c, Cast::ZExt, borrow, s_.t.i32));
    V found = s_.m.cmp(c, pred::kNe, count, s_.m.i32(0));
    found = s_.m.bin(c, Bin::And, found, at_or_below(entry[0], entry[1], low, high));
    found = s_.m.bin(c, Bin::And, found, s_.m.cmp(c, pred::kEq, offset_high, s_.m.i32(0)));
    found = s_.m.bin(c, Bin::And, found, s_.m.cmp(c, pred::kIult, offset, entry[2]));
    if (in_range) found = s_.m.bin(c, Bin::And, found, *in_range);
    const V index = s_.m.select(c, found, entry[3], null_index);
    const V handle = s_.heap_handle(c, index, false, true, {kRawBuffer | kUnorderedView, 0});
    return FlatTarget{handle, s_.m.select(c, found, offset, s_.m.i32(0))};
}

FlatTarget Lower::flat_buffer_target(const IrInst& inst, const BufferFields& fields, std::optional<V> index, V off,
                                     std::uint32_t bytes) {
    const auto* rsrc = std::get_if<ScalarOperand>(&fields.rsrc);
    if (rsrc == nullptr) fail("a buffer descriptor not in SGPRs");
    std::vector<V> words;
    for (std::uint8_t k = 0; k < 3; ++k) {
        const std::optional<Value> read = inst.read(Reg::s(static_cast<std::uint8_t>(rsrc->code + k)));
        if (!read) fail("a buffer descriptor not in SGPRs");
        words.push_back(i32_of(*read));
    }
    const std::size_t c = cur_;
    const V high = s_.m.bin(c, Bin::And, words[1], s_.m.i32(kHighAddressMask));
    V stride = s_.m.bin(c, Bin::LShr, words[1], s_.m.i32(kStrideShift));
    stride = s_.m.bin(c, Bin::And, stride, s_.m.i32(kStrideMask));
    const V records = words[2];
    V byte = off;
    if (index) byte = s_.m.bin(c, Bin::Add, s_.m.bin(c, Bin::Mul, *index, stride), off);
    const V size = s_.m.i32(bytes);
    const V room = s_.m.bin(c, Bin::Sub, records, byte);
    V raw_ok = s_.m.cmp(c, pred::kIult, byte, records);
    raw_ok = s_.m.bin(c, Bin::And, raw_ok, s_.m.cmp(c, pred::kIule, size, room));
    const V unstrided = s_.m.cmp(c, pred::kEq, stride, s_.m.i32(0));
    const V record_ok = index ? s_.m.cmp(c, pred::kIult, *index, records)
                              : s_.m.cmp(c, pred::kNe, records, s_.m.i32(0));
    const V in_range = s_.m.select(c, unstrided, raw_ok, record_ok);
    const auto [low, top] = add_address(words[0], high, byte);
    return flat_lookup(low, top, in_range);
}

void Lower::flat_smem(const IrInst& inst) {
    const auto* fields = std::get_if<SmemFields>(&inst.fields);
    if (fields == nullptr || !fields->data) fail("a scalar load through a runtime address");
    const auto* data = std::get_if<ScalarOperand>(&*fields->data);
    const auto* base = std::get_if<ScalarOperand>(&fields->base);
    const std::string_view name = inst.name();
    constexpr std::string_view kLoad = "s_load_";
    if (data == nullptr || base == nullptr || !name.starts_with(kLoad))
        fail("a scalar memory access through a runtime address");
    const std::optional<std::uint32_t> count = dwords_named(name.substr(kLoad.size()));
    if (!count) fail(name);
    const std::optional<Value> low = inst.read(Reg::s(base->code));
    const std::optional<Value> high = inst.read(Reg::s(static_cast<std::uint8_t>(base->code + 1)));
    if (!low || !high) fail("a scalar address not in SGPRs");
    const std::size_t c = cur_;
    V off = s_.m.i32(static_cast<std::uint32_t>(fields->offset));
    if (const auto* scalar = std::get_if<ScalarOperand>(&fields->soffset); scalar == nullptr || scalar->code != kNull)
        off = s_.m.bin(c, Bin::Add, off, operand(inst, fields->soffset));
    const V top = s_.m.bin(c, Bin::And, i32_of(*high), s_.m.i32(kHighAddressMask));
    const auto [address_low, address_high] = add_address(i32_of(*low), top, off);
    const FlatTarget target = flat_lookup(address_low, address_high, std::nullopt);
    std::optional<V> in_code;
    if (reads_program_counter()) in_code = s_.m.cmp(c, pred::kEq, top, s_.m.i32(kCodeAddressTag));
    for (std::uint32_t first = 0; first < *count; first += kMaxLoadDwords) {
        const std::uint32_t n = std::min(kMaxLoadDwords, *count - first);
        const V at = s_.m.bin(c, Bin::Add, target.byte, s_.m.i32(kDwordBytes * first));
        const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << n) - 1u));
        const V r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                          {target.handle, at, s_.m.undef(s_.t.i32), mask, s_.m.i32(kDwordBytes)});
        for (std::uint32_t k = 0; k < n; ++k) {
            V x = as_int(s_.m.extract(c, r, k));
            if (in_code) {
                const V code_at = s_.m.bin(c, Bin::Add, address_low, s_.m.i32(kDwordBytes * (first + k)));
                x = s_.m.select(c, *in_code, code_dword(code_at), x);
            }
            write(inst, Reg::s(static_cast<std::uint8_t>(data->code + first + k)), x, std::nullopt);
        }
    }
}

bool Lower::reads_program_counter() const {
    for (const IrBlock& block : x_->p->blocks)
        for (const IrInst& candidate : block.insts)
            if (candidate.name() == "s_getpc_b64") return true;
    return false;
}

void Lower::flat_global(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<FlatFields>(&inst.fields);
    if (fields == nullptr || !fields->addr) fail("a global access without an address");
    const auto* addr = std::get_if<VectorOperand>(&*fields->addr);
    if (addr == nullptr) fail("a global address not in VGPRs");
    const std::string_view name = inst.name();
    const std::size_t c = cur_;
    const auto vgpr = [&](std::uint8_t k) {
        const std::optional<Value> read = inst.read(Reg::v(static_cast<std::uint8_t>(addr->reg + k)));
        if (!read) fail("a global address");
        return i32_of(*read);
    };
    V low;
    V high;
    const auto* saddr = fields->saddr ? std::get_if<ScalarOperand>(&*fields->saddr) : nullptr;
    if (saddr != nullptr && saddr->code != kNull) {
        const std::optional<Value> s_low = inst.read(Reg::s(saddr->code));
        const std::optional<Value> s_high = inst.read(Reg::s(static_cast<std::uint8_t>(saddr->code + 1)));
        if (!s_low || !s_high) fail("a global address not in SGPRs");
        std::tie(low, high) = add_address(i32_of(*s_low), i32_of(*s_high), vgpr(0));
    } else {
        low = vgpr(0);
        high = vgpr(1);
    }
    const V offset = s_.m.i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(fields->offset)));
    std::tie(low, high) = add_address(low, high, offset);
    if (fields->offset < 0) high = s_.m.bin(c, Bin::Sub, high, s_.m.i32(1));
    const std::optional<V> guard_value = guard(b, i);
    const V undef = s_.m.undef(s_.t.i32);
    const V align = s_.m.i32(kDwordBytes);
    constexpr std::string_view kLoad = "global_load_";
    constexpr std::string_view kStore = "global_store_";
    constexpr std::string_view kAtomic = "global_atomic_";
    if (name.starts_with(kLoad)) {
        const std::optional<std::uint32_t> count = dwords_named(name.substr(kLoad.size()));
        if (!count || *count > kMaxLoadDwords) fail(name);
        if (!fields->dst) fail("a global load result");
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) fail("a global load result");
        const FlatTarget target = flat_lookup(low, high, std::nullopt);
        const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << *count) - 1u));
        const V r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                          {target.handle, target.byte, undef, mask, align});
        for (std::uint32_t k = 0; k < *count; ++k)
            write(inst, Reg::v(static_cast<std::uint8_t>(dst->reg + k)), as_int(s_.m.extract(c, r, k)),
                  guard_value);
        return;
    }
    if (!fields->data) fail("global access data");
    if (name.starts_with(kStore)) {
        const std::optional<std::uint32_t> count = dwords_named(name.substr(kStore.size()));
        if (!count || *count > kMaxLoadDwords) fail(name);
        const std::vector<V> values = vgprs(inst, *fields->data, static_cast<std::uint8_t>(*count));
        const FlatTarget target = flat_lookup(low, high, std::nullopt);
        std::vector<V> args{target.handle, target.byte, undef};
        args.insert(args.end(), values.begin(), values.end());
        args.resize(3 + kMaxLoadDwords, undef);
        args.push_back(s_.m.i8(static_cast<std::uint8_t>((1u << *count) - 1u)));
        args.push_back(align);
        const auto at = begin_guarded(guard_value);
        s_.op(cur_, "rawBufferStore.i32", Attr::NoUnwind, s_.t.void_type, 140, args);
        end_guarded(at, {});
        return;
    }
    if (!name.starts_with(kAtomic)) fail(name);
    const std::string_view op = name.substr(kAtomic.size());
    if (op == "fmax" || op == "fmin") {
        if (fields->cache.glc) fail("a float atomic whose old value is read");
        const V value = vgprs(inst, *fields->data, 1)[0];
        const FlatTarget target = flat_lookup(low, high, std::nullopt);
        const V positive = s_.m.cmp(c, pred::kIult, value, s_.m.i32(kSignBit));
        const bool max = op == "fmax";
        const V signed_value = s_.m.select(c, positive, value, s_.m.i32(max ? kSignBit : INT32_MAX));
        const V unsigned_value = s_.m.select(c, positive, s_.m.i32(max ? UINT32_MAX : 0u), value);
        const auto at = begin_guarded(guard_value);
        s_.op(cur_, "atomicBinOp.i32", Attr::NoUnwind, s_.t.i32, 78,
              {target.handle, s_.m.i32(max ? kAtomicSignedMax : kAtomicSignedMin), target.byte, undef, undef,
               signed_value});
        s_.op(cur_, "atomicBinOp.i32", Attr::NoUnwind, s_.t.i32, 78,
              {target.handle, s_.m.i32(max ? kAtomicUnsignedMin : kAtomicUnsignedMax), target.byte, undef, undef,
               unsigned_value});
        end_guarded(at, {});
        return;
    }
    const bool subtract = op == "sub";
    const std::optional<std::uint32_t> code = subtract ? std::optional<std::uint32_t>{0} : atomic_code(op);
    if (!code) fail(name);
    const bool exchange = *code == UINT32_MAX;
    std::vector<V> values = vgprs(inst, *fields->data, exchange ? 2 : 1);
    if (subtract) values[0] = s_.m.bin(c, Bin::Sub, s_.m.i32(0), values[0]);
    const FlatTarget target = flat_lookup(low, high, std::nullopt);
    const auto at = begin_guarded(guard_value);
    V result;
    if (exchange) {
        result = s_.op(cur_, "atomicCompareExchange.i32", Attr::NoUnwind, s_.t.i32, 79,
                       {target.handle, target.byte, undef, undef, values[1], values[0]});
    } else {
        result = s_.op(cur_, "atomicBinOp.i32", Attr::NoUnwind, s_.t.i32, 78,
                       {target.handle, s_.m.i32(*code), target.byte, undef, undef, values[0]});
    }
    const bool returns = fields->cache.glc;
    const V old = s_.m.i32(0);
    const V landed = end_guarded(at, {{result, old}})[0];
    if (returns && fields->dst) {
        if (const auto* dst = std::get_if<VectorOperand>(&*fields->dst))
            write(inst, Reg::v(dst->reg), landed, std::nullopt);
    }
}

bool Lower::flat_buffer(const Source& src, const UsedResource* used) const {
    if (src.kind == Source::Kind::Flat) return true;
    if (src.kind == Source::Kind::Loaded) {
        const auto load = scalar_.find(src.load);
        return load != scalar_.end() && load->second.param == kFlat;
    }
    if (src.kind != Source::Kind::Built || src.address.param >= x_->native->layout.size()) return false;
    if (x_->native->layout[src.address.param].root.kind != RootType::Kind::Descriptor) return false;
    return used == nullptr || used->kind.kind == ResourceType::Kind::Uav;
}

}  // namespace g6shim::shader::emit
