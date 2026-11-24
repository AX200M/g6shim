// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "reference.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace g6shim::d3d12_xs::bvh::reference {

namespace {

constexpr std::uint32_t kFlagCompressed = 0x00010000u;
constexpr std::uint32_t kFlagDehydrated = 0x01000000u;
constexpr std::uint32_t kLowByte = 0xFFu;
constexpr std::uint32_t kIdMask = 0x00FFFFFFu;
constexpr std::uint32_t kAlignmentMask = 0xFFu;
constexpr std::size_t kMaxSteps = 1u << 20;
constexpr int kTriSlots[4][3] = {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {2, 4, 0}};

struct Box {
    float lo[3];
    float hi[3];
};

struct Inner {
    int child[4];
    bool inner_child[4];
    int count;
    Box box;
};

struct Ref {
    std::uint32_t leaf;
    std::uint32_t kind;
    std::uint32_t primitive;
};

struct LeafBuild {
    float vertices[4][3];
    std::uint32_t primitive0;
    std::uint32_t primitive1;
    std::uint32_t remap;
};

struct ChildSlot {
    std::uint32_t pointer;
    Box box;
};

struct TriResult {
    double t_num;
    double t_den;
    double i_num;
    double j_num;
};

struct Best {
    bool hit = false;
    double t = 0;
    std::optional<std::uint32_t> instance;
    std::uint32_t geometry = 0;
    std::uint32_t primitive = 0;
    double bary[2]{};
    bool front = false;
};

Box empty_box() {
    Box box;
    const float inf = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        box.lo[axis] = inf;
        box.hi[axis] = -inf;
    }
    return box;
}

bool box_empty(const Box& box) {
    return !(box.lo[0] <= box.hi[0] && box.lo[1] <= box.hi[1] && box.lo[2] <= box.hi[2]);
}

bool box_empty(const float lo[3], const float hi[3]) {
    return !(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]);
}

Box unite(const Box& a, const Box& b) {
    if (box_empty(a)) return b;
    if (box_empty(b)) return a;
    Box out;
    for (int axis = 0; axis < 3; ++axis) {
        out.lo[axis] = std::min(a.lo[axis], b.lo[axis]);
        out.hi[axis] = std::max(a.hi[axis], b.hi[axis]);
    }
    return out;
}

bool any_nan(const float vertices[3][3]) {
    for (int vertex = 0; vertex < 3; ++vertex)
        for (int axis = 0; axis < 3; ++axis)
            if (std::isnan(vertices[vertex][axis])) return true;
    return false;
}

Box triangle_box(const float vertices[3][3]) {
    if (any_nan(vertices)) return empty_box();
    Box box = empty_box();
    for (int vertex = 0; vertex < 3; ++vertex)
        for (int axis = 0; axis < 3; ++axis) {
            box.lo[axis] = std::min(box.lo[axis], vertices[vertex][axis]);
            box.hi[axis] = std::max(box.hi[axis], vertices[vertex][axis]);
        }
    return box;
}

void copy_vertex(float dst[3], const float src[3]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

bool same_vertex(const float a[3], const float b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

std::uint32_t vertex_bits(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x7FFFFFFFu) == 0 ? 0u : bits;
}

struct EdgeKey {
    std::uint32_t bits[6];
    bool operator==(const EdgeKey& other) const {
        for (int i = 0; i < 6; ++i)
            if (bits[i] != other.bits[i]) return false;
        return true;
    }
};

struct EdgeHash {
    std::size_t operator()(const EdgeKey& key) const {
        std::size_t hash = 0;
        for (std::uint32_t bits : key.bits) hash = hash * 1315423911u + bits;
        return hash;
    }
};

EdgeKey edge_key(const float a[3], const float b[3]) {
    EdgeKey key{};
    for (int axis = 0; axis < 3; ++axis) {
        key.bits[axis] = vertex_bits(a[axis]);
        key.bits[3 + axis] = vertex_bits(b[axis]);
    }
    return key;
}

int slot_of(const float order[3][3], const float point[3]) {
    for (int slot = 0; slot < 3; ++slot)
        if (same_vertex(order[slot], point)) return slot;
    return 0;
}

std::uint32_t remap_for(const float tri[3][3], const float order[3][3]) {
    return static_cast<std::uint32_t>(slot_of(order, tri[1])) |
           (static_cast<std::uint32_t>(slot_of(order, tri[2])) << 2);
}

double centre_axis(const Box& box, int axis) {
    if (box_empty(box)) return 0.0;
    const double centre = (static_cast<double>(box.lo[axis]) + static_cast<double>(box.hi[axis])) * 0.5;
    return std::isfinite(centre) ? centre : 0.0;
}

void halve(std::vector<std::uint32_t> group, const std::vector<Box>& boxes,
           std::vector<std::vector<std::uint32_t>>& out) {
    if (group.size() < 2) {
        if (!group.empty()) out.push_back(std::move(group));
        return;
    }
    const double inf = std::numeric_limits<double>::infinity();
    double lo[3] = {inf, inf, inf};
    double hi[3] = {-inf, -inf, -inf};
    for (std::uint32_t item : group) {
        for (int axis = 0; axis < 3; ++axis) {
            const double value = centre_axis(boxes[item], axis);
            lo[axis] = std::min(lo[axis], value);
            hi[axis] = std::max(hi[axis], value);
        }
    }
    int axis = 0;
    double widest = hi[0] - lo[0];
    for (int candidate = 1; candidate < 3; ++candidate) {
        const double extent = hi[candidate] - lo[candidate];
        if (extent > widest) {
            widest = extent;
            axis = candidate;
        }
    }
    std::stable_sort(group.begin(), group.end(), [&](std::uint32_t a, std::uint32_t b) {
        return centre_axis(boxes[a], axis) < centre_axis(boxes[b], axis);
    });
    const std::size_t mid = group.size() / 2;
    out.push_back({group.begin(), group.begin() + static_cast<std::ptrdiff_t>(mid)});
    out.push_back({group.begin() + static_cast<std::ptrdiff_t>(mid), group.end()});
}

std::vector<std::vector<std::uint32_t>> split4(const std::vector<std::uint32_t>& items, const std::vector<Box>& boxes) {
    std::vector<std::vector<std::uint32_t>> halves;
    halve(items, boxes, halves);
    std::vector<std::vector<std::uint32_t>> out;
    for (auto& half : halves) {
        std::vector<std::vector<std::uint32_t>> parts;
        halve(std::move(half), boxes, parts);
        for (auto& part : parts)
            if (!part.empty()) out.push_back(std::move(part));
    }
    return out;
}

int build_tree(std::vector<std::uint32_t> items, const std::vector<Box>& boxes, std::vector<Inner>& inners) {
    Inner node{};
    node.box = empty_box();
    if (items.size() <= 4) {
        node.count = static_cast<int>(items.size());
        for (int i = 0; i < node.count; ++i) {
            node.child[i] = static_cast<int>(items[static_cast<std::size_t>(i)]);
            node.inner_child[i] = false;
            node.box = unite(node.box, boxes[items[static_cast<std::size_t>(i)]]);
        }
        inners.push_back(node);
        return static_cast<int>(inners.size() - 1);
    }
    const auto groups = split4(items, boxes);
    node.count = static_cast<int>(groups.size());
    for (int i = 0; i < node.count; ++i) {
        if (groups[static_cast<std::size_t>(i)].size() == 1) {
            const std::uint32_t item = groups[static_cast<std::size_t>(i)][0];
            node.child[i] = static_cast<int>(item);
            node.inner_child[i] = false;
            node.box = unite(node.box, boxes[item]);
        } else {
            const int id = build_tree(groups[static_cast<std::size_t>(i)], boxes, inners);
            node.child[i] = id;
            node.inner_child[i] = true;
            node.box = unite(node.box, inners[static_cast<std::size_t>(id)].box);
        }
    }
    inners.push_back(node);
    return static_cast<int>(inners.size() - 1);
}

struct Regions {
    std::uint32_t parents;
    std::uint32_t elements;
    std::uint32_t nodes;
    std::uint32_t end;
    std::uint32_t node_size;
};

bool regions_for(std::uint32_t records, std::uint32_t record_size, std::uint32_t inner_count, bool box16, Regions& out) {
    const std::uint32_t node_size = box16 ? kBox16Size : kBox32Size;
    const std::uint64_t body =
        (std::uint64_t{records} * record_size + std::uint64_t{inner_count} * node_size) / kUnit;
    const std::uint64_t parent_bytes = (4 * body + kUnit - 1) / kUnit * kUnit;
    const std::uint64_t elements = kHeaderSize + parent_bytes;
    const std::uint64_t nodes = elements + std::uint64_t{records} * record_size;
    const std::uint64_t end = nodes + std::uint64_t{inner_count} * node_size;
    if (end > (std::uint64_t{1} << 32) - kUnit || end / kUnit >= (std::uint64_t{1} << 29)) return false;
    out.parents = kHeaderSize;
    out.elements = static_cast<std::uint32_t>(elements);
    out.nodes = static_cast<std::uint32_t>(nodes);
    out.end = static_cast<std::uint32_t>(end);
    out.node_size = node_size;
    return true;
}

void write_box_slot(std::uint8_t* node, int slot, const Box& box, bool box16) {
    if (box16) {
        std::uint32_t words[3];
        encode_box16(box.lo, box.hi, words);
        std::memcpy(node + 16 + 12 * slot, words, sizeof(words));
        return;
    }
    float packed[6] = {box.lo[0], box.lo[1], box.lo[2], box.hi[0], box.hi[1], box.hi[2]};
    std::memcpy(node + 16 + 24 * slot, packed, sizeof(packed));
}

void write_inner(std::uint8_t* base, std::uint32_t offset, bool box16, const ChildSlot* kids, int count,
                 std::unordered_map<std::uint32_t, std::uint32_t>& parents) {
    const std::uint32_t self = pointer(offset, box16 ? kBox16 : kBox32);
    std::uint32_t pointers[4];
    for (int slot = 0; slot < 4; ++slot) {
        if (slot < count) {
            pointers[slot] = kids[slot].pointer;
            parents[pointers[slot]] = self;
            write_box_slot(base + offset, slot, kids[slot].box, box16);
        } else {
            pointers[slot] = kInvalid;
            write_box_slot(base + offset, slot, empty_box(), box16);
        }
    }
    std::memcpy(base + offset, pointers, sizeof(pointers));
}

void write_parents(std::uint8_t* base, const Regions& regions,
                   const std::unordered_map<std::uint32_t, std::uint32_t>& parents) {
    const std::uint32_t units = (regions.end - regions.elements) / kUnit;
    for (std::uint32_t unit = 0; unit < units; ++unit) {
        const std::uint32_t invalid = kInvalid;
        std::memcpy(base + regions.parents + 4 * unit, &invalid, sizeof(invalid));
    }
    for (const auto& entry : parents) {
        const std::uint32_t unit = (pointer_offset(entry.first) - regions.elements) / kUnit;
        std::memcpy(base + regions.parents + 4 * unit, &entry.second, sizeof(entry.second));
        if (pointer_type(entry.first) == kInstance)
            std::memcpy(base + pointer_offset(entry.first) + 116, &entry.second, sizeof(entry.second));
    }
}

void write_header(std::uint8_t* base, std::uint32_t flags, const Regions& regions, std::uint32_t count, const Box& bounds,
                  std::uint32_t root) {
    Header header{};
    header.flags = flags;
    header.total_size_in_64b_nodes = regions.end / kUnit - 1;
    header.element_count = count;
    for (int axis = 0; axis < 3; ++axis) {
        header.bounds_min[axis] = bounds.lo[axis];
        header.bounds_max[axis] = bounds.hi[axis];
    }
    header.nodes_offset = regions.nodes;
    header.root = root;
    header.elements_offset = regions.elements;
    header.parents_offset = regions.parents;
    std::memcpy(base, &header, sizeof(header));
}

std::vector<LeafBuild> pair_triangles(std::span<const Triangle> triangles) {
    const std::size_t count = triangles.size();
    std::vector<bool> used(count, false);
    std::vector<bool> inactive(count, false);
    std::unordered_map<EdgeKey, std::vector<std::pair<std::uint32_t, int>>, EdgeHash> edges;
    for (std::size_t i = 0; i < count; ++i) {
        inactive[i] = any_nan(triangles[i].vertices);
        if (inactive[i]) continue;
        for (int edge = 0; edge < 3; ++edge) {
            const float* a = triangles[i].vertices[edge];
            const float* b = triangles[i].vertices[(edge + 1) % 3];
            edges[edge_key(b, a)].push_back({static_cast<std::uint32_t>(i), edge});
        }
    }
    std::vector<LeafBuild> leaves;
    for (std::size_t i = 0; i < count; ++i) {
        if (used[i]) continue;
        used[i] = true;
        int mate_edge = -1;
        std::uint32_t mate = 0;
        int mate_kj = 0;
        if (!inactive[i]) {
            for (int edge = 0; edge < 3 && mate_edge < 0; ++edge) {
                const float* a = triangles[i].vertices[edge];
                const float* b = triangles[i].vertices[(edge + 1) % 3];
                const auto found = edges.find(edge_key(a, b));
                if (found == edges.end()) continue;
                for (const auto& candidate : found->second) {
                    if (used[candidate.first] || inactive[candidate.first]) continue;
                    mate_edge = edge;
                    mate = candidate.first;
                    mate_kj = candidate.second;
                    break;
                }
            }
        }
        LeafBuild leaf{};
        leaf.primitive1 = kInvalid;
        leaf.remap = kRemapIdentity;
        if (mate_edge < 0) {
            copy_vertex(leaf.vertices[0], triangles[i].vertices[0]);
            copy_vertex(leaf.vertices[1], triangles[i].vertices[1]);
            copy_vertex(leaf.vertices[2], triangles[i].vertices[2]);
            copy_vertex(leaf.vertices[3], triangles[i].vertices[2]);
            leaf.primitive0 = static_cast<std::uint32_t>(i);
            leaves.push_back(leaf);
            continue;
        }
        used[mate] = true;
        const Triangle& primary = triangles[i];
        const Triangle& secondary = triangles[mate];
        float order[3][3];
        copy_vertex(order[0], primary.vertices[(mate_edge + 2) % 3]);
        copy_vertex(order[1], primary.vertices[mate_edge]);
        copy_vertex(order[2], primary.vertices[(mate_edge + 1) % 3]);
        float mate_order[3][3];
        copy_vertex(mate_order[0], order[1]);
        copy_vertex(mate_order[1], secondary.vertices[(mate_kj + 2) % 3]);
        copy_vertex(mate_order[2], order[2]);
        copy_vertex(leaf.vertices[0], order[0]);
        copy_vertex(leaf.vertices[1], order[1]);
        copy_vertex(leaf.vertices[2], order[2]);
        copy_vertex(leaf.vertices[3], mate_order[1]);
        leaf.primitive0 = static_cast<std::uint32_t>(i);
        leaf.primitive1 = mate;
        leaf.remap = remap_for(primary.vertices, order) | (remap_for(secondary.vertices, mate_order) << 8);
        leaves.push_back(leaf);
    }
    return leaves;
}

std::vector<LeafBuild> single_triangles(std::span<const Triangle> triangles) {
    std::vector<LeafBuild> leaves;
    leaves.reserve(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        LeafBuild leaf{};
        copy_vertex(leaf.vertices[0], triangles[i].vertices[0]);
        copy_vertex(leaf.vertices[1], triangles[i].vertices[1]);
        copy_vertex(leaf.vertices[2], triangles[i].vertices[2]);
        copy_vertex(leaf.vertices[3], triangles[i].vertices[2]);
        leaf.primitive0 = static_cast<std::uint32_t>(i);
        leaf.primitive1 = kInvalid;
        leaf.remap = kRemapIdentity;
        leaves.push_back(leaf);
    }
    return leaves;
}

Box world_box(const float transform[12], const float lo[3], const float hi[3], std::uint32_t mask, std::uint64_t blas) {
    if (blas == 0 || (mask & kLowByte) == 0 || box_empty(lo, hi)) return empty_box();
    Box box = empty_box();
    for (int corner = 0; corner < 8; ++corner) {
        float point[3];
        for (int axis = 0; axis < 3; ++axis) point[axis] = (corner & (1 << axis)) != 0 ? hi[axis] : lo[axis];
        for (int row = 0; row < 3; ++row) {
            const float value = transform[row * 4] * point[0] + transform[row * 4 + 1] * point[1] +
                                transform[row * 4 + 2] * point[2] + transform[row * 4 + 3];
            box.lo[row] = std::min(box.lo[row], value);
            box.hi[row] = std::max(box.hi[row], value);
        }
    }
    return box;
}

bool identity_rotation(const float transform[12]) {
    return transform[0] == 1.f && transform[1] == 0.f && transform[2] == 0.f && transform[4] == 0.f &&
           transform[5] == 1.f && transform[6] == 0.f && transform[8] == 0.f && transform[9] == 0.f &&
           transform[10] == 1.f;
}

struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    bool load(std::uint64_t offset, void* dst, std::size_t count) const {
        if (count == 0) return true;
        if (dst == nullptr || offset > size || count > size - static_cast<std::size_t>(offset)) return false;
        std::memcpy(dst, data + offset, count);
        return true;
    }
    bool u32(std::uint64_t offset, std::uint32_t& value) const { return load(offset, &value, 4); }
    bool u32s(std::uint64_t offset, std::uint32_t* values, std::size_t count) const {
        return load(offset, values, count * 4);
    }
    bool f32s(std::uint64_t offset, float* values, std::size_t count) const { return load(offset, values, count * 4); }
};

std::uint64_t wide_offset(std::uint32_t encoded) { return (std::uint64_t{encoded} >> kPointerUnitShift) * kUnit; }

bool read_u32s(const Memory& memory, std::uint64_t address, std::uint32_t* values, std::size_t count) {
    return memory.read(address, count * 4, values);
}

bool read_f32s(const Memory& memory, std::uint64_t address, float* values, std::size_t count) {
    return memory.read(address, count * 4, values);
}

double inverse_component(double component) {
    if (component == 0.0) return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), component);
    return 1.0 / component;
}

double box_test(double extent, const double origin[3], const double direction[3], const double inverse[3], const float lo[3],
                const float hi[3]) {
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return std::numeric_limits<double>::infinity();
    double tnear = 0.0;
    double tfar = extent;
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return std::numeric_limits<double>::infinity();
        } else {
            const double t1 = (static_cast<double>(lo[axis]) - origin[axis]) * inverse[axis];
            const double t2 = (static_cast<double>(hi[axis]) - origin[axis]) * inverse[axis];
            tnear = std::max(tnear, std::min(t1, t2));
            tfar = std::min(tfar, std::max(t1, t2));
            if (tfar < tnear) return std::numeric_limits<double>::infinity();
        }
    }
    return tnear;
}

bool tie_miss(double ay, double by, double cy, bool winding) {
    if (by == 0.0 && cy == 0.0) return ay > 0.0;
    const bool below = by < 0.0 || (by == 0.0 && cy > 0.0);
    return below != winding;
}

double edge_weight(int selector, double u, double v, double w) {
    if (selector == 1) return v;
    if (selector == 2) return w;
    return u;
}

TriResult triangle_test(const double vertices[3][3], const double origin[3], const double direction[3], std::uint32_t remap) {
    const double inf = std::numeric_limits<double>::infinity();
    TriResult miss{inf, 1.0, 0.0, 0.0};
    const double magnitude[3] = {std::abs(direction[0]), std::abs(direction[1]), std::abs(direction[2])};
    const double major = std::max(magnitude[0], std::max(magnitude[1], magnitude[2]));
    int perm[3];
    if (magnitude[0] == major) {
        perm[0] = 1;
        perm[1] = 2;
        perm[2] = 0;
    } else if (magnitude[1] == major) {
        perm[0] = 2;
        perm[1] = 0;
        perm[2] = 1;
    } else {
        perm[0] = 0;
        perm[1] = 1;
        perm[2] = 2;
    }
    double relative[3][3];
    double dir[3];
    for (int axis = 0; axis < 3; ++axis) dir[axis] = direction[perm[axis]];
    for (int vertex = 0; vertex < 3; ++vertex)
        for (int axis = 0; axis < 3; ++axis) relative[vertex][axis] = vertices[vertex][perm[axis]] - origin[perm[axis]];
    double shear[3][3];
    for (int vertex = 0; vertex < 3; ++vertex) {
        shear[vertex][0] = relative[vertex][0] * dir[2] - dir[0] * relative[vertex][2];
        shear[vertex][1] = relative[vertex][1] * dir[2] - dir[1] * relative[vertex][2];
        shear[vertex][2] = relative[vertex][2];
    }
    const double u = shear[2][0] * shear[1][1] - shear[2][1] * shear[1][0];
    const double v = shear[0][0] * shear[2][1] - shear[0][1] * shear[2][0];
    const double w = shear[1][0] * shear[0][1] - shear[1][1] * shear[0][0];
    const double t_num = u * shear[0][2] + v * shear[1][2] + w * shear[2][2];
    const double t_den = (u + v + w) * dir[2];
    bool missed = (u < 0.0 || v < 0.0 || w < 0.0) && (u > 0.0 || v > 0.0 || w > 0.0);
    missed = missed || t_den == 0.0;
    missed = missed || t_num * std::copysign(1.0, t_den) <= 0.0;
    missed = missed || std::isnan(t_num);
    const bool winding = t_den > 0.0;
    if (u == 0.0) missed = missed || tie_miss(shear[0][1], shear[1][1], shear[2][1], winding);
    if (v == 0.0) missed = missed || tie_miss(shear[1][1], shear[2][1], shear[0][1], winding);
    if (w == 0.0) missed = missed || tie_miss(shear[2][1], shear[0][1], shear[1][1], winding);
    if (missed) return miss;
    const double i_num = edge_weight(static_cast<int>(remap & 3u), u, v, w) * dir[2];
    const double j_num = edge_weight(static_cast<int>((remap >> 2) & 3u), u, v, w) * dir[2];
    return {t_num, t_den, i_num, j_num};
}

bool intersect_box(const Memory& memory, std::uint64_t structure, std::uint32_t encoded, const double origin[3],
                   const double direction[3], const double inverse[3], double extent, std::uint32_t kids[4]) {
    const std::uint64_t address = structure + wide_offset(encoded);
    const std::uint32_t kind = pointer_type(encoded);
    std::uint32_t pointers[4];
    if (!read_u32s(memory, address, pointers, 4)) return false;
    double distance[4];
    const double inf = std::numeric_limits<double>::infinity();
    for (int slot = 0; slot < 4; ++slot) {
        float lo[3];
        float hi[3];
        if (kind == kBox16) {
            std::uint32_t words[3];
            if (!read_u32s(memory, address + 16 + 12 * static_cast<std::uint32_t>(slot), words, 3)) return false;
            decode_box16(words, lo, hi);
        } else {
            float packed[6];
            if (!read_f32s(memory, address + 16 + 24 * static_cast<std::uint32_t>(slot), packed, 6)) return false;
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = packed[axis];
                hi[axis] = packed[3 + axis];
            }
        }
        distance[slot] = box_test(extent, origin, direction, inverse, lo, hi);
        kids[slot] = distance[slot] != inf && distance[slot] <= extent ? pointers[slot] : kInvalid;
    }
    constexpr int kPairs[5][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}, {1, 2}};
    for (const auto& pair : kPairs) {
        if (distance[pair[0]] > distance[pair[1]]) {
            std::swap(distance[pair[0]], distance[pair[1]]);
            std::swap(kids[pair[0]], kids[pair[1]]);
        }
    }
    return true;
}

void accept_triangle(const Memory& memory, std::uint64_t structure, std::uint32_t encoded, const TriResult& result, Best& best,
                     double tmax, std::optional<std::uint32_t> instance) {
    const double inf = std::numeric_limits<double>::infinity();
    if (result.t_num == inf) return;
    const double t = result.t_num / result.t_den;
    if (t > (best.hit ? best.t : tmax)) return;
    const std::uint64_t leaf = structure + wide_offset(encoded);
    std::uint32_t words[3];
    if (!read_u32s(memory, leaf + 48, words, 3)) return;
    best.hit = true;
    best.t = t;
    best.instance = instance;
    best.geometry = words[0] >> 8;
    best.primitive = pointer_type(encoded) == kTri1 ? words[2] : words[1];
    best.bary[0] = result.i_num / result.t_den;
    best.bary[1] = result.j_num / result.t_den;
    best.front = result.t_den > 0.0;
}

void trace_blas(const Memory& memory, std::uint64_t structure, const double origin[3], const double direction[3], double tmax,
                Best& best, std::optional<std::uint32_t> instance) {
    std::uint32_t root = 0;
    if (!read_u32s(memory, structure + 48, &root, 1)) return;
    const double inverse[3] = {inverse_component(direction[0]), inverse_component(direction[1]),
                               inverse_component(direction[2])};
    std::vector<std::uint32_t> stack;
    stack.push_back(root);
    std::unordered_set<std::uint32_t> seen;
    std::size_t steps = 0;
    while (!stack.empty() && steps < kMaxSteps) {
        const std::uint32_t encoded = stack.back();
        stack.pop_back();
        if (encoded == kInvalid) continue;
        if (!seen.insert(encoded).second) continue;
        ++steps;
        const std::uint32_t kind = pointer_type(encoded);
        const double extent = best.hit ? best.t : tmax;
        if (kind <= kTri3) {
            const std::uint64_t address = structure + wide_offset(encoded);
            double vertices[3][3]{};
            bool loaded = true;
            for (int slot = 0; slot < 3; ++slot) {
                float packed[3]{};
                const std::uint32_t vertex = static_cast<std::uint32_t>(kTriSlots[kind][slot]);
                if (!read_f32s(memory, address + 12ull * vertex, packed, 3)) loaded = false;
                for (int axis = 0; axis < 3; ++axis) vertices[slot][axis] = packed[axis];
            }
            std::uint32_t remap_word = 0;
            if (!loaded || !read_u32s(memory, address + 60, &remap_word, 1)) continue;
            const std::uint32_t remap = (remap_word >> (8 * kind)) & 0xFFu;
            accept_triangle(memory, structure, encoded, triangle_test(vertices, origin, direction, remap), best, tmax,
                            instance);
            continue;
        }
        if (kind != kBox16 && kind != kBox32) continue;
        std::uint32_t kids[4];
        if (!intersect_box(memory, structure, encoded, origin, direction, inverse, extent, kids)) continue;
        for (int slot = 3; slot >= 0; --slot)
            if (kids[slot] != kInvalid) stack.push_back(kids[slot]);
    }
}

void transform_point(const float matrix[12], const double point[3], double out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = static_cast<double>(matrix[row * 4]) * point[0] + static_cast<double>(matrix[row * 4 + 1]) * point[1] +
                   static_cast<double>(matrix[row * 4 + 2]) * point[2] + static_cast<double>(matrix[row * 4 + 3]);
}

void transform_dir(const float matrix[12], const double direction[3], double out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = static_cast<double>(matrix[row * 4]) * direction[0] +
                   static_cast<double>(matrix[row * 4 + 1]) * direction[1] +
                   static_cast<double>(matrix[row * 4 + 2]) * direction[2];
}

std::optional<Hit> to_hit(const Best& best) {
    if (!best.hit) return std::nullopt;
    Hit hit;
    hit.t = static_cast<float>(best.t);
    hit.instance = best.instance;
    hit.geometry = best.geometry;
    hit.primitive = best.primitive;
    hit.barycentrics[0] = static_cast<float>(best.bary[0]);
    hit.barycentrics[1] = static_cast<float>(best.bary[1]);
    hit.front = best.front;
    return hit;
}

bool contained(const float parent_lo[3], const float parent_hi[3], const float lo[3], const float hi[3]) {
    for (int axis = 0; axis < 3; ++axis)
        if (!(parent_lo[axis] <= lo[axis] && hi[axis] <= parent_hi[axis])) return false;
    return true;
}

bool point_inside(const float lo[3], const float hi[3], const float point[3]) {
    for (int axis = 0; axis < 3; ++axis)
        if (!(lo[axis] <= point[axis] && point[axis] <= hi[axis])) return false;
    return true;
}

struct WalkItem {
    std::uint32_t encoded = 0;
    bool has_box = false;
    float lo[3]{};
    float hi[3]{};
    std::optional<std::uint32_t> parent;
};

}  // namespace

std::vector<std::uint8_t> build_blas(std::span<const Triangle> triangles, const BlasOptions& options) {
    if (triangles.empty() || triangles.size() > std::numeric_limits<std::uint32_t>::max()) return {};
    const auto leaves = options.pair ? pair_triangles(triangles) : single_triangles(triangles);
    std::vector<Ref> refs;
    std::vector<Box> boxes;
    refs.reserve(triangles.size());
    for (std::size_t leaf = 0; leaf < leaves.size(); ++leaf) {
        Ref primary{static_cast<std::uint32_t>(leaf), kTri0, leaves[leaf].primitive0};
        float vertices[3][3];
        copy_vertex(vertices[0], triangles[primary.primitive].vertices[0]);
        copy_vertex(vertices[1], triangles[primary.primitive].vertices[1]);
        copy_vertex(vertices[2], triangles[primary.primitive].vertices[2]);
        refs.push_back(primary);
        boxes.push_back(triangle_box(vertices));
        if (leaves[leaf].primitive1 != kInvalid) {
            Ref secondary{static_cast<std::uint32_t>(leaf), kTri1, leaves[leaf].primitive1};
            copy_vertex(vertices[0], triangles[secondary.primitive].vertices[0]);
            copy_vertex(vertices[1], triangles[secondary.primitive].vertices[1]);
            copy_vertex(vertices[2], triangles[secondary.primitive].vertices[2]);
            refs.push_back(secondary);
            boxes.push_back(triangle_box(vertices));
        }
    }
    std::vector<std::uint32_t> items(refs.size());
    for (std::uint32_t i = 0; i < items.size(); ++i) items[i] = i;
    std::vector<Inner> inners;
    const int root = build_tree(std::move(items), boxes, inners);
    Regions regions{};
    if (!regions_for(static_cast<std::uint32_t>(leaves.size()), kLeafSize, static_cast<std::uint32_t>(inners.size()),
                     options.box16, regions))
        return {};
    std::vector<std::uint8_t> buffer(regions.end);
    const std::uint32_t geometry = (options.geometry_index << 8) | (options.geometry_flags & kLowByte);
    for (std::size_t leaf = 0; leaf < leaves.size(); ++leaf) {
        Leaf record{};
        for (int slot = 0; slot < 4; ++slot)
            for (int axis = 0; axis < 3; ++axis) record.vertices[slot][axis] = leaves[leaf].vertices[slot][axis];
        record.geometry = geometry;
        record.primitive0 = leaves[leaf].primitive0;
        record.primitive1 = leaves[leaf].primitive1;
        record.remap = leaves[leaf].remap;
        std::memcpy(buffer.data() + regions.elements + leaf * kLeafSize, &record, sizeof(record));
    }
    std::unordered_map<std::uint32_t, std::uint32_t> parents;
    for (std::size_t node = 0; node < inners.size(); ++node) {
        ChildSlot kids[4];
        const Inner& inner = inners[node];
        for (int slot = 0; slot < inner.count; ++slot) {
            if (inner.inner_child[slot]) {
                const int id = inner.child[slot];
                kids[slot].pointer = pointer(regions.nodes + static_cast<std::uint32_t>(id) * regions.node_size,
                                             options.box16 ? kBox16 : kBox32);
                kids[slot].box = inners[static_cast<std::size_t>(id)].box;
            } else {
                const Ref& ref = refs[static_cast<std::size_t>(inner.child[slot])];
                kids[slot].pointer = pointer(regions.elements + ref.leaf * kLeafSize, ref.kind);
                kids[slot].box = boxes[static_cast<std::size_t>(inner.child[slot])];
            }
        }
        write_inner(buffer.data(), regions.nodes + static_cast<std::uint32_t>(node) * regions.node_size, options.box16, kids,
                    inner.count, parents);
    }
    const std::uint32_t root_pointer =
        pointer(regions.nodes + static_cast<std::uint32_t>(root) * regions.node_size, options.box16 ? kBox16 : kBox32);
    write_parents(buffer.data(), regions, parents);
    write_header(buffer.data(), kFlagBottomLevel | kVersion2403, regions, static_cast<std::uint32_t>(refs.size()),
                 inners[static_cast<std::size_t>(root)].box, root_pointer);
    return buffer;
}

std::vector<std::uint8_t> build_tlas(std::span<const InstanceInput> instances, bool box16) {
    if (instances.empty() || instances.size() > std::numeric_limits<std::uint32_t>::max()) return {};
    for (const InstanceInput& instance : instances) {
        if ((instance.blas_address & kAlignmentMask) != 0) return {};
        float inverse[12];
        if (!invert_affine(instance.transform, inverse)) return {};
    }
    std::vector<Box> boxes(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i)
        boxes[i] = world_box(instances[i].transform, instances[i].blas_min, instances[i].blas_max, instances[i].mask,
                             instances[i].blas_address);
    std::vector<std::uint32_t> items(instances.size());
    for (std::uint32_t i = 0; i < items.size(); ++i) items[i] = i;
    std::vector<Inner> inners;
    const int root = build_tree(std::move(items), boxes, inners);
    Regions regions{};
    if (!regions_for(static_cast<std::uint32_t>(instances.size()), kInstanceSize, static_cast<std::uint32_t>(inners.size()),
                     box16, regions))
        return {};
    std::vector<std::uint8_t> buffer(regions.end);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const InstanceInput& instance = instances[i];
        float inverse[12];
        if (!invert_affine(instance.transform, inverse)) return {};
        InstanceRecord record{};
        for (int component = 0; component < 12; ++component) {
            record.object_to_world[component] = instance.transform[component];
            record.world_to_object[component] = inverse[component];
        }
        record.id_and_mask = (instance.instance_id & kIdMask) | ((instance.mask & kLowByte) << 24);
        record.contribution_and_flags = (instance.contribution & kIdMask) | ((instance.flags & kLowByte) << 24);
        record.root_and_flags = (instance.blas_root & ~kInstanceFlagsMask) |
                                (identity_rotation(instance.transform) ? kInstanceIdentityRotation : 0u);
        record.blas_address_256 = static_cast<std::uint32_t>(instance.blas_address >> 8);
        record.index = static_cast<std::uint32_t>(i);
        record.parent = kInvalid;
        std::memcpy(buffer.data() + regions.elements + i * kInstanceSize, &record, sizeof(record));
    }
    std::unordered_map<std::uint32_t, std::uint32_t> parents;
    for (std::size_t node = 0; node < inners.size(); ++node) {
        ChildSlot kids[4];
        const Inner& inner = inners[node];
        for (int slot = 0; slot < inner.count; ++slot) {
            if (inner.inner_child[slot]) {
                const int id = inner.child[slot];
                kids[slot].pointer =
                    pointer(regions.nodes + static_cast<std::uint32_t>(id) * regions.node_size, box16 ? kBox16 : kBox32);
                kids[slot].box = inners[static_cast<std::size_t>(id)].box;
            } else {
                const std::uint32_t handle = static_cast<std::uint32_t>(inner.child[slot]);
                kids[slot].pointer = pointer(regions.elements + handle * kInstanceSize, kInstance);
                kids[slot].box = boxes[handle];
            }
        }
        write_inner(buffer.data(), regions.nodes + static_cast<std::uint32_t>(node) * regions.node_size, box16, kids,
                    inner.count, parents);
    }
    const std::uint32_t root_pointer =
        pointer(regions.nodes + static_cast<std::uint32_t>(root) * regions.node_size, box16 ? kBox16 : kBox32);
    write_parents(buffer.data(), regions, parents);
    write_header(buffer.data(), kVersion2403, regions, static_cast<std::uint32_t>(instances.size()),
                 inners[static_cast<std::size_t>(root)].box, root_pointer);
    return buffer;
}

void Memory::place(std::uint64_t address, std::span<const std::uint8_t> bytes) {
    regions_.push_back(Region{address, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
}

bool Memory::read(std::uint64_t address, std::size_t count, void* out) const {
    if (count == 0) return true;
    if (out == nullptr) return false;
    for (const Region& region : regions_) {
        if (address < region.address) continue;
        const std::uint64_t offset = address - region.address;
        if (offset > region.bytes.size() || count > region.bytes.size() - static_cast<std::size_t>(offset)) continue;
        std::memcpy(out, region.bytes.data() + offset, count);
        return true;
    }
    return false;
}

std::optional<Hit> trace(const Memory& memory, std::uint64_t structure, const Ray& ray) {
    std::uint32_t flags = 0;
    if (!read_u32s(memory, structure, &flags, 1)) return std::nullopt;
    const double origin[3] = {ray.origin[0], ray.origin[1], ray.origin[2]};
    const double direction[3] = {ray.direction[0], ray.direction[1], ray.direction[2]};
    Best best;
    if ((flags & kFlagBottomLevel) != 0) {
        trace_blas(memory, structure, origin, direction, ray.t_max, best, std::nullopt);
        return to_hit(best);
    }
    std::uint32_t root = 0;
    if (!read_u32s(memory, structure + 48, &root, 1)) return std::nullopt;
    const double inverse[3] = {inverse_component(direction[0]), inverse_component(direction[1]),
                               inverse_component(direction[2])};
    std::vector<std::uint32_t> stack;
    stack.push_back(root);
    std::unordered_set<std::uint32_t> seen;
    std::size_t steps = 0;
    while (!stack.empty() && steps < kMaxSteps) {
        const std::uint32_t encoded = stack.back();
        stack.pop_back();
        if (encoded == kInvalid) continue;
        if (!seen.insert(encoded).second) continue;
        ++steps;
        if (pointer_type(encoded) == kInstance) {
            const std::uint64_t record = structure + wide_offset(encoded);
            std::uint32_t words[4];
            if (!read_u32s(memory, record + 48, words, 4)) continue;
            const std::uint32_t instance_mask = words[0] >> 24;
            if ((instance_mask & ray.mask) == 0 || words[3] == 0) continue;
            float matrix[12];
            std::uint32_t index = 0;
            std::uint32_t blas_root = 0;
            const std::uint64_t blas = std::uint64_t{words[3]} << 8;
            if (!read_f32s(memory, record + 64, matrix, 12) || !read_u32s(memory, record + 112, &index, 1) ||
                !read_u32s(memory, blas + 48, &blas_root, 1))
                continue;
            if (blas_root != (words[2] & ~kInstanceFlagsMask)) continue;
            double local_origin[3];
            double local_direction[3];
            transform_point(matrix, origin, local_origin);
            transform_dir(matrix, direction, local_direction);
            trace_blas(memory, blas, local_origin, local_direction, ray.t_max, best, index);
            continue;
        }
        if (pointer_type(encoded) != kBox16 && pointer_type(encoded) != kBox32) continue;
        std::uint32_t kids[4];
        if (!intersect_box(memory, structure, encoded, origin, direction, inverse, best.hit ? best.t : ray.t_max, kids))
            continue;
        for (int slot = 3; slot >= 0; --slot)
            if (kids[slot] != kInvalid) stack.push_back(kids[slot]);
    }
    return to_hit(best);
}

std::optional<Hit> brute_force(std::span<const SceneInstance> scene, const Ray& ray) {
    Best best;
    const double origin[3] = {ray.origin[0], ray.origin[1], ray.origin[2]};
    const double direction[3] = {ray.direction[0], ray.direction[1], ray.direction[2]};
    for (std::size_t index = 0; index < scene.size(); ++index) {
        const SceneInstance& instance = scene[index];
        double local_origin[3] = {origin[0], origin[1], origin[2]};
        double local_direction[3] = {direction[0], direction[1], direction[2]};
        std::optional<std::uint32_t> instance_id;
        if (instance.transform) {
            if ((instance.mask & ray.mask) == 0) continue;
            float inverse[12];
            if (!invert_affine(instance.transform->data(), inverse)) continue;
            transform_point(inverse, origin, local_origin);
            transform_dir(inverse, direction, local_direction);
            instance_id = static_cast<std::uint32_t>(index);
        }
        for (std::size_t primitive = 0; primitive < instance.triangles.size(); ++primitive) {
            double vertices[3][3];
            for (int vertex = 0; vertex < 3; ++vertex)
                for (int axis = 0; axis < 3; ++axis)
                    vertices[vertex][axis] = instance.triangles[primitive].vertices[vertex][axis];
            const TriResult result = triangle_test(vertices, local_origin, local_direction, kRemapIdentity);
            if (result.t_num == std::numeric_limits<double>::infinity()) continue;
            const double t = result.t_num / result.t_den;
            if (t > (best.hit ? best.t : static_cast<double>(ray.t_max))) continue;
            best.hit = true;
            best.t = t;
            best.instance = instance_id;
            best.geometry = 0;
            best.primitive = static_cast<std::uint32_t>(primitive);
            best.bary[0] = result.i_num / result.t_den;
            best.bary[1] = result.j_num / result.t_den;
            best.front = result.t_den > 0.0;
        }
    }
    return to_hit(best);
}

std::vector<std::string> validate(std::span<const std::uint8_t> data, std::optional<bool> bottom_level) {
    std::vector<std::string> errors;
    if (data.size() < kHeaderSize) return {"buffer shorter than the 64-byte header"};
    Bytes bytes{data.data(), data.size()};
    Header header{};
    if (!bytes.load(0, &header, sizeof(header))) return {"buffer shorter than the 64-byte header"};
    const bool bottom = (header.flags & kFlagBottomLevel) != 0;
    if (bottom_level && bottom != *bottom_level) errors.push_back("bottom-level flag does not match the expected level");
    if ((header.flags & (kFlagCompressed | kFlagDehydrated)) != 0) {
        errors.push_back("compressed or dehydrated BVH is not a runtime tree");
        return errors;
    }
    const std::uint64_t covered = (std::uint64_t{header.total_size_in_64b_nodes} + 1) * kUnit;
    if (covered != data.size()) errors.push_back("totalSizeIn64BNodes does not cover the buffer");
    if ((header.nodes_offset % kUnit) != 0 || header.nodes_offset > data.size())
        errors.push_back("offsetToNodes is unaligned or past the end");
    if ((header.elements_offset % kUnit) != 0 || header.elements_offset > data.size())
        errors.push_back("offsetToElements is unaligned or past the end");
    if (!errors.empty()) return errors;
    const std::int32_t parents = static_cast<std::int32_t>(header.parents_offset);
    std::unordered_set<std::uint32_t> seen;
    std::vector<WalkItem> stack;
    stack.push_back(WalkItem{header.root, false, {}, {}, std::nullopt});
    std::size_t leaves = 0;
    std::size_t steps = 0;
    while (!stack.empty() && steps < kMaxSteps) {
        const WalkItem item = stack.back();
        stack.pop_back();
        ++steps;
        if (item.encoded == kInvalid) continue;
        const std::uint32_t kind = pointer_type(item.encoded);
        const std::uint64_t offset = wide_offset(item.encoded);
        const std::uint64_t record = kind == kBox32 ? kBox32Size : kind == kInstance ? kInstanceSize : kUnit;
        if (offset + record > data.size() || offset < kHeaderSize) {
            errors.push_back("node outside the buffer");
            continue;
        }
        if (!seen.insert(item.encoded).second) {
            errors.push_back("node reached twice");
            continue;
        }
        if (kind > kTri3 && parents != 0) {
            const std::int64_t unit = (static_cast<std::int64_t>(offset) - static_cast<std::int64_t>(header.elements_offset)) /
                                      static_cast<std::int64_t>(kUnit);
            const std::int64_t entry = static_cast<std::int64_t>(parents) + 4 * unit;
            if (entry + 4 > static_cast<std::int64_t>(data.size())) {
                errors.push_back("parent table entry is outside the buffer");
            } else if (entry >= static_cast<std::int64_t>(kHeaderSize) &&
                       entry + 4 <= static_cast<std::int64_t>(header.elements_offset)) {
                std::uint32_t stored = 0;
                const std::uint32_t expected = item.parent ? *item.parent : kInvalid;
                if (!bytes.u32(static_cast<std::uint64_t>(entry), stored) || stored != expected)
                    errors.push_back("parent table entry does not match");
            }
        }
        if (kind <= kTri3) {
            if (!bottom) errors.push_back("triangle node in a top-level BVH");
            if (kind >= kTri2) errors.push_back("TRI2 or TRI3 overlaps leaf metadata");
            bool inactive = false;
            float points[3][3]{};
            for (int slot = 0; slot < 3; ++slot) {
                const int vertex = kTriSlots[kind][slot];
                if (!bytes.f32s(offset + 12ull * static_cast<std::uint32_t>(vertex), points[slot], 3)) {
                    errors.push_back("triangle vertices outside the buffer");
                    inactive = true;
                    break;
                }
                for (int axis = 0; axis < 3; ++axis)
                    if (std::isnan(points[slot][axis])) inactive = true;
            }
            if (item.has_box && !inactive) {
                for (int slot = 0; slot < 3; ++slot) {
                    if (!point_inside(item.lo, item.hi, points[slot])) {
                        errors.push_back("triangle is not inside its parent box");
                        break;
                    }
                }
            }
            ++leaves;
            continue;
        }
        if (kind == kInstance) {
            if (bottom) errors.push_back("instance node in a bottom-level BVH");
            std::uint32_t blas = 0;
            if (!bytes.u32(offset + 60, blas)) errors.push_back("instance record outside the buffer");
            if (blas == 0 && item.has_box && !box_empty(item.lo, item.hi))
                errors.push_back("instance has no BLAS address but a non-empty box");
            ++leaves;
            continue;
        }
        if (kind == kBox16 || kind == kBox32) {
            std::uint32_t kids[4];
            if (!bytes.u32s(offset, kids, 4)) {
                errors.push_back("box node outside the buffer");
                continue;
            }
            for (int slot = 0; slot < 4; ++slot) {
                if (kids[slot] == kInvalid) continue;
                float lo[3];
                float hi[3];
                if (kind == kBox16) {
                    std::uint32_t words[3];
                    if (!bytes.u32s(offset + 16 + 12ull * static_cast<std::uint32_t>(slot), words, 3)) {
                        errors.push_back("box node outside the buffer");
                        continue;
                    }
                    decode_box16(words, lo, hi);
                } else {
                    float packed[6];
                    if (!bytes.f32s(offset + 16 + 24ull * static_cast<std::uint32_t>(slot), packed, 6)) {
                        errors.push_back("box node outside the buffer");
                        continue;
                    }
                    for (int axis = 0; axis < 3; ++axis) {
                        lo[axis] = packed[axis];
                        hi[axis] = packed[3 + axis];
                    }
                }
                if (item.has_box && !contained(item.lo, item.hi, lo, hi))
                    errors.push_back("child box is not inside its parent box");
                WalkItem child;
                child.encoded = kids[slot];
                child.has_box = true;
                child.parent = item.encoded;
                for (int axis = 0; axis < 3; ++axis) {
                    child.lo[axis] = lo[axis];
                    child.hi[axis] = hi[axis];
                }
                stack.push_back(child);
            }
            continue;
        }
        errors.push_back("node has type AABB");
    }
    if (steps >= kMaxSteps) errors.push_back("traversal stopped");
    if (leaves != header.element_count) errors.push_back("numElements does not match the reachable leaves");
    return errors;
}

std::vector<std::string> box16_rounding(std::span<const std::uint8_t> data) {
    std::vector<std::string> errors;
    if (data.size() < kHeaderSize) return {"buffer shorter than the 64-byte header"};
    Bytes bytes{data.data(), data.size()};
    std::uint32_t root = 0;
    if (!bytes.u32(48, root) || root == kInvalid) return errors;
    std::unordered_map<std::uint32_t, Box> memo;
    std::unordered_set<std::uint32_t> visiting;
    struct Job {
        std::uint32_t encoded;
        int phase;
    };
    std::vector<Job> stack;
    stack.push_back({root, 0});
    std::size_t steps = 0;
    while (!stack.empty() && steps < kMaxSteps) {
        Job job = stack.back();
        ++steps;
        if (memo.find(job.encoded) != memo.end()) {
            stack.pop_back();
            continue;
        }
        const std::uint32_t kind = pointer_type(job.encoded);
        const std::uint64_t offset = wide_offset(job.encoded);
        const std::uint64_t record = kind == kBox32 ? kBox32Size : kUnit;
        if (offset + record > data.size() || offset < kHeaderSize) {
            errors.push_back("node outside the buffer");
            memo[job.encoded] = empty_box();
            stack.pop_back();
            continue;
        }
        if (kind <= kTri3) {
            float points[3][3]{};
            bool inactive = false;
            bool loaded = true;
            for (int slot = 0; slot < 3; ++slot) {
                const int vertex = kTriSlots[kind][slot];
                if (!bytes.f32s(offset + 12ull * static_cast<std::uint32_t>(vertex), points[slot], 3)) loaded = false;
                for (int axis = 0; axis < 3 && loaded; ++axis)
                    if (std::isnan(points[slot][axis])) inactive = true;
            }
            memo[job.encoded] = !loaded || inactive ? empty_box() : triangle_box(points);
            stack.pop_back();
            continue;
        }
        if (kind != kBox16 && kind != kBox32) {
            memo[job.encoded] = empty_box();
            stack.pop_back();
            continue;
        }
        std::uint32_t kids[4];
        if (!bytes.u32s(offset, kids, 4)) {
            errors.push_back("box node outside the buffer");
            memo[job.encoded] = empty_box();
            stack.pop_back();
            continue;
        }
        if (job.phase == 0) {
            if (!visiting.insert(job.encoded).second) {
                errors.push_back("node reached twice");
                memo[job.encoded] = empty_box();
                stack.pop_back();
                continue;
            }
            stack.back().phase = 1;
            for (int slot = 3; slot >= 0; --slot)
                if (kids[slot] != kInvalid) stack.push_back({kids[slot], 0});
            continue;
        }
        Box box = empty_box();
        for (std::uint32_t child : kids) {
            if (child == kInvalid) continue;
            const auto found = memo.find(child);
            if (found != memo.end()) box = unite(box, found->second);
        }
        if (kind == kBox16) {
            for (int slot = 0; slot < 4; ++slot) {
                Box want_box = empty_box();
                if (kids[slot] != kInvalid) {
                    const auto found = memo.find(kids[slot]);
                    if (found != memo.end()) want_box = found->second;
                }
                std::uint32_t want[3];
                encode_box16(want_box.lo, want_box.hi, want);
                std::uint32_t got[3]{};
                if (!bytes.u32s(offset + 16 + 12ull * static_cast<std::uint32_t>(slot), got, 3) || got[0] != want[0] ||
                    got[1] != want[1] || got[2] != want[2])
                    errors.push_back("BOX16 child does not match the exact box");
            }
        }
        visiting.erase(job.encoded);
        memo[job.encoded] = box;
        stack.pop_back();
    }
    if (steps >= kMaxSteps) errors.push_back("traversal stopped");
    return errors;
}

bool invert_affine(const float transform[12], float out[12]) {
    if (transform == nullptr || out == nullptr) return false;
    double linear[3][3];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) linear[row][column] = transform[row * 4 + column];
    const double det = linear[0][0] * (linear[1][1] * linear[2][2] - linear[1][2] * linear[2][1]) -
                       linear[0][1] * (linear[1][0] * linear[2][2] - linear[1][2] * linear[2][0]) +
                       linear[0][2] * (linear[1][0] * linear[2][1] - linear[1][1] * linear[2][0]);
    if (det == 0.0) return false;
    double inverse[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const int row1 = (column + 1) % 3;
            const int row2 = (column + 2) % 3;
            const int column1 = (row + 1) % 3;
            const int column2 = (row + 2) % 3;
            inverse[row][column] =
                (linear[row1][column1] * linear[row2][column2] - linear[row1][column2] * linear[row2][column1]) / det;
        }
    }
    const double translation[3] = {transform[3], transform[7], transform[11]};
    for (int row = 0; row < 3; ++row) {
        out[row * 4] = static_cast<float>(inverse[row][0]);
        out[row * 4 + 1] = static_cast<float>(inverse[row][1]);
        out[row * 4 + 2] = static_cast<float>(inverse[row][2]);
        const double shifted =
            -(inverse[row][0] * translation[0] + inverse[row][1] * translation[1] + inverse[row][2] * translation[2]);
        out[row * 4 + 3] = static_cast<float>(shifted);
    }
    return true;
}

}  // namespace g6shim::d3d12_xs::bvh::reference
