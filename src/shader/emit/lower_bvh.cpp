// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lower_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace g6shim::shader::emit {

namespace {

constexpr std::uint32_t kNoChild = 0xFFFF'FFFFu;
constexpr std::uint32_t kBoxSortBit = 1u << 31;
constexpr std::uint32_t kBaseHighMask = 0xFF;
constexpr std::uint32_t kBaseShift = 8;
constexpr std::uint32_t kKindMask = 7;
constexpr std::uint32_t kBoxKindMask = 6;
constexpr std::uint32_t kBox16Kind = 4;
constexpr std::uint32_t kBox32Kind = 5;
constexpr std::uint32_t kFirstNonTriangleKind = 4;
constexpr std::uint32_t kPointerToNodeShift = 3;
constexpr std::uint32_t kNodeBytes = 64;
constexpr std::uint32_t kChildren = 4;
constexpr std::uint32_t kBoxFloats = 24;
constexpr std::uint32_t kBoxBytesStart = 16;
constexpr std::uint32_t kHalfBoxDwords = 12;
constexpr std::uint32_t kTriangleIdByte = 60;
constexpr std::uint32_t kVertexBytes = 12;
constexpr std::uint32_t kMaxLoadDwords = 4;
constexpr std::uint32_t kDwordBytes = 4;
constexpr std::uint32_t kFMax = 35;
constexpr std::uint32_t kFMin = 36;
constexpr std::uint32_t kFAbs = 6;
constexpr std::uint32_t kLegacyF16ToF32 = 131;
constexpr std::uint32_t kFloatMaxBits = 0x7F7F'FFFFu;
constexpr std::uint32_t kFloatLowestBits = 0xFF7F'FFFFu;
constexpr std::uint32_t kInfinityBits = 0x7F80'0000u;
constexpr std::array<std::uint32_t, 4> kMissBits = {kInfinityBits, 0x3F80'0000u, 0, 0};
constexpr std::array<std::uint32_t, 4> kVertexMaps = {0x210, 0x231, 0x432, 0x042};
constexpr std::array<std::pair<std::size_t, std::size_t>, 5> kSortNetwork = {
    std::pair<std::size_t, std::size_t>{0, 1}, {2, 3}, {0, 2}, {1, 3}, {1, 2}};
constexpr std::size_t kPointerOperands = 1;
constexpr std::size_t kRayOperands = 10;

}  // namespace

BvhNode Lower::bvh_node_at(V low, V high) {
    const FlatTarget target = flat_lookup(low, high, std::nullopt);
    return BvhNode{target.handle, target.byte, low, high, target.found};
}

std::vector<V> Lower::bvh_words(const BvhNode& node, std::optional<V> base, std::uint32_t offset,
                                std::uint32_t count) {
    const std::size_t c = cur_;
    const V undef = s_.m.undef(s_.t.i32);
    const V align = s_.m.i32(kDwordBytes);
    std::vector<V> out;
    out.reserve(count);
    for (std::uint32_t start = 0; start < count; start += kMaxLoadDwords) {
        const std::uint32_t n = std::min(kMaxLoadDwords, count - start);
        V byte = node.byte;
        if (base) byte = s_.m.bin(c, Bin::Add, byte, *base);
        byte = s_.m.bin(c, Bin::Add, byte, s_.m.i32(offset + kDwordBytes * start));
        const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << n) - 1u));
        const V r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                          {node.handle, byte, undef, mask, align});
        for (std::uint32_t k = 0; k < n; ++k) out.push_back(as_int(s_.m.extract(c, r, k)));
    }
    return out;
}

void Lower::bvh(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<ImageFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (fields->a16 || fields->d16 || fields->tfe || fields->lwe || fields->dmask != 0xF)
        fail("a ray-tracing node test with 16-bit operands or a partial result");
    const bool wide = inst.name() == "image_bvh64_intersect_ray";
    std::vector<std::uint8_t> regs;
    for (const Operand& a : fields->addr)
        if (const auto* vector = std::get_if<VectorOperand>(&a))
            for (std::uint8_t k = 0; k < vector->dwords; ++k) regs.push_back(static_cast<std::uint8_t>(vector->reg + k));
    const std::size_t pointer_words = kPointerOperands + (wide ? 1 : 0);
    if (regs.size() != pointer_words + kRayOperands) fail("a ray-tracing node test's operands");
    const auto* data = std::get_if<VectorOperand>(&fields->data);
    if (data == nullptr) fail("a ray-tracing node test's result");
    const auto* rsrc = std::get_if<ScalarOperand>(&fields->rsrc);
    if (rsrc == nullptr) fail("a ray-tracing structure descriptor not in SGPRs");
    std::array<V, 2> words{};
    for (std::uint8_t k = 0; k < words.size(); ++k) {
        const std::optional<Value> read = inst.read(Reg::s(static_cast<std::uint8_t>(rsrc->code + k)));
        if (!read) fail("a ray-tracing structure descriptor not in SGPRs");
        words[k] = i32_of(*read);
    }
    const std::optional<V> guard_value = guard(b, i);
    std::vector<V> x;
    for (const std::uint8_t r : regs) {
        const std::optional<Value> read = inst.read(Reg::v(r));
        if (!read) fail("a ray-tracing node test's operands");
        x.push_back(i32_of(*read));
    }
    const V pointer = x[0];
    const V pointer_high = wide ? x[1] : s_.m.i32(0);
    std::vector<V> f;
    for (std::size_t k = pointer_words; k < x.size(); ++k) f.push_back(as_float(x[k]));
    const BvhRay ray{f[0], {f[1], f[2], f[3]}, {f[4], f[5], f[6]}, {f[7], f[8], f[9]}};

    const std::size_t c = cur_;
    const V base_low = s_.m.bin(c, Bin::Shl, words[0], s_.m.i32(kBaseShift));
    const V base_high = s_.m.bin(
        c, Bin::Or,
        s_.m.bin(c, Bin::Shl, s_.m.bin(c, Bin::And, words[1], s_.m.i32(kBaseHighMask)), s_.m.i32(kBaseShift)),
        s_.m.bin(c, Bin::LShr, words[0], s_.m.i32(32 - kBaseShift)));
    const V node_low = s_.m.bin(c, Bin::Shl, s_.m.bin(c, Bin::And, pointer, s_.m.i32(~kKindMask)),
                                s_.m.i32(kPointerToNodeShift));
    const V node_high = s_.m.bin(c, Bin::Or, s_.m.bin(c, Bin::Shl, pointer_high, s_.m.i32(kPointerToNodeShift)),
                                 s_.m.bin(c, Bin::LShr, pointer, s_.m.i32(32 - kPointerToNodeShift)));
    const auto [low, carry] = add_sub_carry(c, true, base_low, node_low, std::nullopt);
    V high = s_.m.bin(c, Bin::Add, base_high, node_high);
    high = s_.m.bin(c, Bin::Add, high, s_.m.cast(c, Cast::ZExt, carry, s_.t.i32));
    const BvhNode node = bvh_node_at(low, high);
    const V sort = s_.m.cmp(c, pred::kNe, s_.m.bin(c, Bin::And, words[1], s_.m.i32(kBoxSortBit)), s_.m.i32(0));

    const V zero = s_.m.i32(0);
    const auto gate = begin_guarded(guard_value);
    const std::size_t head = cur_;
    const V kind = s_.m.bin(head, Bin::And, pointer, s_.m.i32(kKindMask));
    const V pair = s_.m.bin(head, Bin::And, pointer, s_.m.i32(kBoxKindMask));
    const V is_box = s_.m.cmp(head, pred::kEq, pair, s_.m.i32(kBox16Kind));
    const V is_triangle = s_.m.cmp(head, pred::kIult, kind, s_.m.i32(kFirstNonTriangleKind));
    const std::size_t box_block = s_.m.new_block();
    const std::size_t leaf_block = s_.m.new_block();
    const std::size_t triangle_block = s_.m.new_block();
    const std::size_t join = s_.m.new_block();
    s_.m.terminate(head, Term::cond_br(is_box, box_block, leaf_block));
    s_.m.terminate(leaf_block, Term::cond_br(is_triangle, triangle_block, join));
    cur_ = box_block;
    const std::array<V, 4> boxes = bvh_boxes(node, kind, ray, sort);
    const std::size_t box_end = cur_;
    s_.m.terminate(box_end, Term::br(join));
    cur_ = triangle_block;
    const std::array<V, 4> triangle = bvh_triangle(node, kind, ray);
    const std::size_t triangle_end = cur_;
    s_.m.terminate(triangle_end, Term::br(join));
    cur_ = join;
    std::vector<std::pair<V, V>> results;
    for (std::size_t k = 0; k < kMissBits.size(); ++k) {
        const V phi = s_.m.phi(join, s_.t.i32);
        s_.m.set_phi(phi, {{boxes[k], box_end}, {triangle[k], triangle_end}, {s_.m.i32(kMissBits[k]), leaf_block}});
        results.emplace_back(phi, zero);
    }
    const std::vector<V> out = end_guarded(gate, results);
    for (std::size_t k = 0; k < out.size(); ++k)
        write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), out[k], guard_value);
}

std::array<V, 4> Lower::bvh_boxes(const BvhNode& node, V kind, const BvhRay& ray, V sort) {
    const std::size_t head = cur_;
    const std::vector<V> children = bvh_words(node, std::nullopt, 0, kChildren);
    const V wide = s_.m.cmp(head, pred::kEq, kind, s_.m.i32(kBox32Kind));
    const std::size_t wide_block = s_.m.new_block();
    const std::size_t half_block = s_.m.new_block();
    const std::size_t join = s_.m.new_block();
    s_.m.terminate(head, Term::cond_br(wide, wide_block, half_block));

    cur_ = wide_block;
    std::vector<V> words = bvh_words(node, std::nullopt, kBoxBytesStart, kHalfBoxDwords);
    const auto [second_low, second_high] = add_address(node.low, node.high, s_.m.i32(kNodeBytes));
    const BvhNode second = bvh_node_at(second_low, second_high);
    const std::vector<V> rest = bvh_words(second, std::nullopt, 0, kHalfBoxDwords);
    words.insert(words.end(), rest.begin(), rest.end());
    std::vector<V> floats;
    for (const V w : words) floats.push_back(as_float(w));
    const std::size_t wide_end = cur_;
    s_.m.terminate(wide_end, Term::br(join));

    cur_ = half_block;
    const std::vector<V> packed = bvh_words(node, std::nullopt, kBoxBytesStart, kHalfBoxDwords);
    std::vector<V> halves;
    for (std::uint32_t n = 0; n < kBoxFloats; ++n) {
        V h = packed[n / 2];
        if (n % 2 == 1) h = s_.m.bin(half_block, Bin::LShr, h, s_.m.i32(16));
        halves.push_back(s_.op(half_block, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, kLegacyF16ToF32, {h}));
    }
    s_.m.terminate(half_block, Term::br(join));

    cur_ = join;
    const V second_found = s_.m.phi(join, s_.m.type_of(node.found));
    s_.m.set_phi(second_found, {{second.found, wide_end}, {node.found, half_block}});
    const V both_found = s_.m.bin(join, Bin::And, node.found, second_found);
    const std::array<V, 4> there = {node.found, node.found, both_found, both_found};
    std::vector<V> coords;
    for (std::uint32_t n = 0; n < kBoxFloats; ++n) {
        const V phi = s_.m.phi(join, s_.t.f32);
        s_.m.set_phi(phi, {{floats[n], wide_end}, {halves[n], half_block}});
        coords.push_back(phi);
    }
    const V most = s_.m.f32(kFloatMaxBits);
    const V least = s_.m.f32(kFloatLowestBits);
    std::array<V, 3> inverse{};
    for (std::size_t a = 0; a < 3; ++a) inverse[a] = binary(kFMin, binary(kFMax, ray.inverse[a], least), most);
    const std::size_t c = join;
    const V none = s_.m.i32(kNoChild);
    const V infinity = s_.m.f32(kInfinityBits);
    const V fzero = s_.m.f32(0);
    const V on = s_.m.boolean(true);
    std::array<V, 4> ids{};
    std::array<V, 4> distances{};
    for (std::size_t n = 0; n < kChildren; ++n) {
        const V* bx = &coords[6 * n];
        std::array<V, 3> near{};
        std::array<V, 3> far{};
        for (std::size_t a = 0; a < 3; ++a) {
            const V low = s_.m.bin(c, Bin::Mul, s_.m.bin(c, Bin::Sub, bx[a], ray.origin[a]), inverse[a]);
            const V high = s_.m.bin(c, Bin::Mul, s_.m.bin(c, Bin::Sub, bx[3 + a], ray.origin[a]), inverse[a]);
            near[a] = binary(kFMin, low, high);
            far[a] = binary(kFMax, low, high);
        }
        const V enter = binary(kFMax, binary(kFMax, near[0], near[1]), near[2]);
        const V leave = binary(kFMin, binary(kFMin, far[0], far[1]), far[2]);
        const V from = binary(kFMax, enter, fzero);
        const V through = s_.m.cmp(c, pred::kOge, leave, from);
        const V before = s_.m.cmp(c, pred::kOlt, enter, ray.extent);
        const V box_there = s_.m.bin(c, Bin::Xor, s_.m.cmp(c, pred::kUno, bx[0], bx[0]), on);
        V hit = s_.m.bin(c, Bin::And, s_.m.bin(c, Bin::And, through, before), box_there);
        hit = s_.m.bin(c, Bin::And, hit, s_.m.cmp(c, pred::kNe, children[n], none));
        hit = s_.m.bin(c, Bin::And, hit, there[n]);
        ids[n] = s_.m.select(c, hit, children[n], none);
        distances[n] = s_.m.select(c, hit, enter, infinity);
    }
    for (const auto& [a, b] : kSortNetwork) {
        const V closer = s_.m.cmp(c, pred::kOlt, distances[b], distances[a]);
        const V swap = s_.m.bin(c, Bin::And, closer, sort);
        const V da = distances[a];
        const V db = distances[b];
        distances[a] = s_.m.select(c, swap, db, da);
        distances[b] = s_.m.select(c, swap, da, db);
        const V ia = ids[a];
        const V ib = ids[b];
        ids[a] = s_.m.select(c, swap, ib, ia);
        ids[b] = s_.m.select(c, swap, ia, ib);
    }
    return ids;
}

std::array<V, 4> Lower::bvh_triangle(const BvhNode& node, V kind, const BvhRay& ray) {
    const std::size_t c = cur_;
    const V k2 = s_.m.i32(2);
    const V k3 = s_.m.i32(3);
    V id = bvh_words(node, std::nullopt, kTriangleIdByte, 1)[0];
    id = s_.m.bin(c, Bin::LShr, id, s_.m.bin(c, Bin::Shl, kind, k3));
    const V i = s_.m.bin(c, Bin::And, id, k3);
    const V j = s_.m.bin(c, Bin::And, s_.m.bin(c, Bin::LShr, id, k2), k3);
    const V first = s_.m.bin(c, Bin::And, s_.m.bin(c, Bin::Sub, s_.m.bin(c, Bin::Sub, k3, i), j), k3);
    V map = s_.m.i32(kVertexMaps[3]);
    for (std::uint32_t k = 3; k-- > 0;)
        map = s_.m.select(c, s_.m.cmp(c, pred::kEq, kind, s_.m.i32(k)), s_.m.i32(kVertexMaps[k]), map);
    std::array<std::array<V, 3>, 3> p{};
    const std::array<V, 3> picks = {first, i, j};
    for (std::size_t n = 0; n < picks.size(); ++n) {
        V vertex = s_.m.bin(c, Bin::LShr, map, s_.m.bin(c, Bin::Shl, picks[n], k2));
        vertex = s_.m.bin(c, Bin::And, vertex, s_.m.i32(0xF));
        const V byte = s_.m.bin(c, Bin::Mul, vertex, s_.m.i32(kVertexBytes));
        const std::vector<V> words = bvh_words(node, byte, 0, 3);
        for (std::size_t a = 0; a < 3; ++a) p[n][a] = s_.m.bin(c, Bin::Sub, as_float(words[a]), ray.origin[a]);
    }
    const V fzero = s_.m.f32(0);
    const V on = s_.m.boolean(true);
    std::array<V, 3> size{};
    for (std::size_t a = 0; a < 3; ++a) size[a] = unary(kFAbs, ray.direction[a]);
    const V xy = s_.m.cmp(c, pred::kOge, size[0], size[1]);
    const V xz = s_.m.cmp(c, pred::kOge, size[0], size[2]);
    const V yz = s_.m.cmp(c, pred::kOge, size[1], size[2]);
    const V z0 = s_.m.bin(c, Bin::And, xy, xz);
    const V z1 = s_.m.bin(c, Bin::And, s_.m.bin(c, Bin::Xor, xy, on), yz);
    const auto pick = [&](const std::array<V, 3>& q, std::size_t n) {
        const V rest = s_.m.select(c, z1, q[(1 + n) % 3], q[(2 + n) % 3]);
        return s_.m.select(c, z0, q[n % 3], rest);
    };
    const V backwards = s_.m.cmp(c, pred::kOlt, pick(ray.direction, 0), fzero);
    const auto axes = [&](const std::array<V, 3>& q) {
        const V x = pick(q, 1);
        const V y = pick(q, 2);
        return std::array<V, 3>{s_.m.select(c, backwards, y, x), s_.m.select(c, backwards, x, y), pick(q, 0)};
    };
    const std::array<V, 3> d = axes(ray.direction);
    const V sz = pick(ray.inverse, 0);
    const V sx = s_.m.bin(c, Bin::Mul, d[0], sz);
    const V sy = s_.m.bin(c, Bin::Mul, d[1], sz);
    std::array<std::array<V, 3>, 3> sheared{};
    for (std::size_t n = 0; n < 3; ++n) {
        const std::array<V, 3> r = axes(p[n]);
        sheared[n][0] = s_.m.bin(c, Bin::Sub, r[0], s_.m.bin(c, Bin::Mul, sx, r[2]));
        sheared[n][1] = s_.m.bin(c, Bin::Sub, r[1], s_.m.bin(c, Bin::Mul, sy, r[2]));
        sheared[n][2] = s_.m.bin(c, Bin::Mul, sz, r[2]);
    }
    const auto cross = [&](V a, V b, V e, V g) {
        return s_.m.bin(c, Bin::Sub, s_.m.bin(c, Bin::Mul, a, b), s_.m.bin(c, Bin::Mul, e, g));
    };
    const auto& [ax, ay, az] = sheared[0];
    const auto& [bx, by, bz] = sheared[1];
    const auto& [cx, cy, cz] = sheared[2];
    const V u = cross(cx, by, cy, bx);
    const V v = cross(ax, cy, ay, cx);
    const V w = cross(bx, ay, by, ax);
    const auto any = [&](std::uint8_t predicate) {
        const V a = s_.m.cmp(c, predicate, u, fzero);
        const V b = s_.m.cmp(c, predicate, v, fzero);
        const V e = s_.m.cmp(c, predicate, w, fzero);
        return s_.m.bin(c, Bin::Or, s_.m.bin(c, Bin::Or, a, b), e);
    };
    const V outside = s_.m.bin(c, Bin::And, any(pred::kOlt), any(pred::kOgt));
    const V inside = s_.m.bin(c, Bin::Xor, outside, on);
    const V det = s_.m.bin(c, Bin::Add, u, s_.m.bin(c, Bin::Add, v, w));
    V t = s_.m.bin(c, Bin::Add, s_.m.bin(c, Bin::Mul, u, az), s_.m.bin(c, Bin::Mul, v, bz));
    t = s_.m.bin(c, Bin::Add, t, s_.m.bin(c, Bin::Mul, w, cz));
    const V front = s_.m.bin(c, Bin::And, s_.m.cmp(c, pred::kOgt, det, fzero), s_.m.cmp(c, pred::kOge, t, fzero));
    const V back = s_.m.bin(c, Bin::And, s_.m.cmp(c, pred::kOlt, det, fzero), s_.m.cmp(c, pred::kOle, t, fzero));
    V hit = s_.m.bin(c, Bin::And, inside, s_.m.bin(c, Bin::Or, front, back));
    hit = s_.m.bin(c, Bin::And, hit, node.found);
    const std::array<V, 4> out = {t, det, v, w};
    std::array<V, 4> result{};
    for (std::size_t n = 0; n < out.size(); ++n)
        result[n] = as_int(s_.m.select(c, hit, out[n], s_.m.f32(kMissBits[n])));
    return result;
}

}  // namespace g6shim::shader::emit
