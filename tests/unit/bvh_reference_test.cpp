// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "format.hpp"
#include "reference.hpp"

#include <array>
#include <bit>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace g6shim::d3d12_xs::bvh;
using namespace g6shim::d3d12_xs::bvh::reference;

namespace {

Triangle triangle(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz) {
    Triangle out{};
    out.vertices[0][0] = ax;
    out.vertices[0][1] = ay;
    out.vertices[0][2] = az;
    out.vertices[1][0] = bx;
    out.vertices[1][1] = by;
    out.vertices[1][2] = bz;
    out.vertices[2][0] = cx;
    out.vertices[2][1] = cy;
    out.vertices[2][2] = cz;
    return out;
}

Header header_of(const std::vector<std::uint8_t>& data) {
    Header header{};
    if (data.size() >= sizeof(header)) std::memcpy(&header, data.data(), sizeof(header));
    return header;
}

bool close_hit(const std::optional<Hit>& traced, const std::optional<Hit>& brute) {
    if (!traced || !brute) return !traced && !brute;
    if (traced->primitive != brute->primitive || traced->instance != brute->instance) return false;
    const float scale = std::max(1.f, std::abs(traced->t));
    return std::abs(traced->t - brute->t) <= 1e-4f * scale;
}

std::vector<Triangle> random_triangles(std::mt19937& rng, int count, float spread) {
    std::uniform_real_distribution<float> centre(-spread, spread);
    std::uniform_real_distribution<float> jitter(-0.6f, 0.6f);
    std::vector<Triangle> triangles;
    triangles.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float origin[3] = {centre(rng), centre(rng), centre(rng)};
        triangles.push_back(triangle(origin[0] + jitter(rng), origin[1] + jitter(rng), origin[2] + jitter(rng),
                                     origin[0] + jitter(rng), origin[1] + jitter(rng), origin[2] + jitter(rng),
                                     origin[0] + jitter(rng), origin[1] + jitter(rng), origin[2] + jitter(rng)));
    }
    return triangles;
}

std::vector<Triangle> grid_mesh(int n, float size) {
    std::vector<std::array<float, 3>> points;
    points.reserve(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(n + 1));
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            points.push_back({(static_cast<float>(x) / static_cast<float>(n) - 0.5f) * size,
                              (static_cast<float>(y) / static_cast<float>(n) - 0.5f) * size,
                              0.1f * static_cast<float>((x * 7 + y * 3) % 5)});
        }
    }
    auto at = [&](int x, int y) {
        const auto& point = points[static_cast<std::size_t>(y * (n + 1) + x)];
        return point;
    };
    std::vector<Triangle> triangles;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto a = at(x, y);
            const auto b = at(x + 1, y);
            const auto c = at(x + 1, y + 1);
            const auto d = at(x, y + 1);
            triangles.push_back(triangle(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]));
            triangles.push_back(triangle(a[0], a[1], a[2], c[0], c[1], c[2], d[0], d[1], d[2]));
        }
    }
    return triangles;
}

void expect_layout(std::uint32_t count, bool top, bool box16, std::uint32_t size, std::uint32_t elements,
                   std::uint32_t nodes, std::uint32_t node_count, std::uint64_t scratch, std::uint32_t entries) {
    Layout layout{};
    CHECK(compute_layout(count, top, box16, &layout));
    CHECK(layout.size == size);
    CHECK(layout.elements_offset == elements);
    CHECK(layout.nodes_offset == nodes);
    CHECK(layout.nodes == node_count);
    CHECK(layout.scratch == scratch);
    CHECK(layout.parent_entries == entries);
    CHECK(layout.parents_offset == kHeaderSize);
    CHECK(layout.element_size == (top ? kInstanceSize : kLeafSize));
    CHECK(layout.node_size == (box16 ? kBox16Size : kBox32Size));
    CHECK(layout.size % kUnit == 0);
}

bool same_scene(const std::vector<std::uint8_t>& data, std::uint64_t address, const std::vector<Triangle>& triangles,
                std::mt19937& rng, int rays) {
    CHECK(validate(data, true).empty());
    CHECK(box16_rounding(data).empty());
    Memory memory;
    memory.place(address, data);
    SceneInstance scene;
    scene.mask = 0xFFu;
    scene.triangles = triangles;
    std::uniform_real_distribution<float> origin_dist(-6.f, 6.f);
    std::uniform_real_distribution<float> aim(-3.f, 3.f);
    int hits = 0;
    for (int i = 0; i < rays; ++i) {
        Ray ray{};
        ray.origin[0] = origin_dist(rng);
        ray.origin[1] = origin_dist(rng);
        ray.origin[2] = origin_dist(rng);
        const float target[3] = {aim(rng), aim(rng), aim(rng)};
        ray.direction[0] = target[0] - ray.origin[0];
        ray.direction[1] = target[1] - ray.origin[1];
        ray.direction[2] = target[2] - ray.origin[2];
        ray.t_max = std::numeric_limits<float>::infinity();
        ray.mask = 0xFFu;
        const auto traced = trace(memory, address, ray);
        const auto brute = brute_force(std::span<const SceneInstance>(&scene, 1), ray);
        if (!close_hit(traced, brute)) return false;
        if (traced) ++hits;
    }
    return hits > 0;
}

}  // namespace

TEST(bvh_half_directed_edges) {
    const float zero = 0.f;
    const float neg_zero = -0.f;
    const float inf = std::numeric_limits<float>::infinity();
    struct Case {
        float value;
        bool up;
        std::uint16_t bits;
    };
    const Case cases[] = {
        {zero, false, 0x0000},
        {zero, true, 0x0000},
        {neg_zero, false, 0x8000},
        {neg_zero, true, 0x8000},
        {1.f, false, 0x3C00},
        {1.f, true, 0x3C00},
        {1.0001f, false, 0x3C00},
        {1.0001f, true, 0x3C01},
        {-1.0001f, false, 0xBC01},
        {-1.0001f, true, 0xBC00},
        {65504.f, true, 0x7BFF},
        {65505.f, false, 0x7BFF},
        {65505.f, true, 0x7C00},
        {-65505.f, false, 0xFC00},
        {-65505.f, true, 0xFBFF},
        {inf, false, 0x7BFF},
        {-inf, true, 0xFBFF},
        {0x1p-14f, false, 0x0400},
        {0x1p-14f, true, 0x0400},
        {3e-5f, false, 0x0000},
        {3e-5f, true, 0x0400},
        {-3e-5f, false, 0x8400},
        {-3e-5f, true, 0x8000},
        {1e-30f, true, 0x0400},
        {-1e-30f, false, 0x8400},
    };
    for (const Case& item : cases) CHECK(half_directed(item.value, item.up) == item.bits);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> exponent(-40.f, -10.f);
    std::uniform_int_distribution<int> sign(0, 1);
    for (int i = 0; i < 2000; ++i) {
        const float magnitude = std::ldexp(1.f, static_cast<int>(exponent(rng)));
        const float value = sign(rng) != 0 ? -magnitude : magnitude;
        for (bool up : {false, true}) {
            const std::uint16_t bits = half_directed(value, up);
            CHECK((bits & 0x7C00) != 0 || (bits & 0x03FF) == 0);
            float lo[3] = {value, value, value};
            float hi[3] = {value, value, value};
            std::uint32_t words[3];
            encode_box16(lo, hi, words);
            float decoded_lo[3];
            float decoded_hi[3];
            decode_box16(words, decoded_lo, decoded_hi);
            CHECK(decoded_lo[0] <= value && value <= decoded_hi[0]);
        }
    }
    std::uniform_int_distribution<std::uint32_t> bits_dist;
    for (int i = 0; i < 4000; ++i) {
        const float value = std::bit_cast<float>(bits_dist(rng));
        if (!std::isfinite(value)) continue;
        float lo[3] = {value, 0.f, -value};
        float hi[3] = {value, 1.f, value};
        std::uint32_t words[3];
        encode_box16(lo, hi, words);
        float decoded_lo[3];
        float decoded_hi[3];
        decode_box16(words, decoded_lo, decoded_hi);
        CHECK(decoded_lo[0] <= lo[0] && decoded_lo[1] <= lo[1] && decoded_lo[2] <= lo[2]);
        CHECK(decoded_hi[0] >= hi[0] && decoded_hi[1] >= hi[1] && decoded_hi[2] >= hi[2]);
    }
}

TEST(bvh_layout_sizes) {
    expect_layout(0, false, false, 256, 128, 128, 1, 96, 2);
    expect_layout(0, false, true, 256, 128, 128, 1, 96, 2);
    expect_layout(0, true, false, 256, 128, 128, 1, 96, 2);
    expect_layout(0, true, true, 256, 128, 128, 1, 96, 2);
    expect_layout(1, false, false, 320, 128, 192, 1, 268, 3);
    expect_layout(1, false, true, 320, 128, 192, 1, 268, 3);
    expect_layout(1, true, false, 384, 128, 256, 1, 268, 4);
    expect_layout(1, true, true, 384, 128, 256, 1, 268, 4);
    expect_layout(2, false, false, 384, 128, 256, 1, 340, 4);
    expect_layout(2, false, true, 384, 128, 256, 1, 340, 4);
    expect_layout(2, true, false, 512, 128, 384, 1, 340, 6);
    expect_layout(2, true, true, 512, 128, 384, 1, 340, 6);
    expect_layout(1000, false, false, 169984, 10112, 74112, 749, 88556, 2498);
    expect_layout(1000, false, true, 119104, 7104, 71104, 749, 88556, 1750);
    expect_layout(1000, true, false, 237952, 14080, 142080, 749, 88556, 3498);
    expect_layout(1000, true, true, 187072, 11072, 139072, 749, 88556, 2750);
    Layout overflow{};
    CHECK(!compute_layout(25264514u, false, false, &overflow));
    CHECK(compute_layout(25264513u, false, false, &overflow));
    CHECK(!compute_layout(40000000u, true, true, &overflow));
}

TEST(bvh_single_triangle_and_empty_inputs) {
    CHECK(build_blas({}, {}).empty());
    CHECK(build_tlas({}, false).empty());
    const Triangle tri = triangle(0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f);
    const auto data = build_blas(std::span<const Triangle>(&tri, 1), {});
    CHECK(!data.empty());
    CHECK(validate(data, true).empty());
    const Header header = header_of(data);
    Box32Node root{};
    std::memcpy(&root, data.data() + pointer_offset(header.root), sizeof(root));
    CHECK(root.children[1] == kInvalid && root.children[2] == kInvalid && root.children[3] == kInvalid);
    CHECK(root.boxes[1][0] > root.boxes[1][3]);
    Memory memory;
    memory.place(0, data);
    Ray ray{{0.25f, 0.25f, -2.f}, {0.f, 0.f, 1.f}, 10.f, 0xFFu};
    const auto hit = trace(memory, 0, ray);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->primitive == 0);
        CHECK(std::abs(hit->t - 2.f) < 1e-5f);
        CHECK(std::abs(hit->barycentrics[0] - 0.25f) < 1e-4f);
        CHECK(std::abs(hit->barycentrics[1] - 0.25f) < 1e-4f);
    }
    Ray miss{{2.f, 2.f, -2.f}, {0.f, 0.f, 1.f}, 10.f, 0xFFu};
    CHECK(!trace(memory, 0, miss));
    Ray behind{{0.25f, 0.25f, 2.f}, {0.f, 0.f, 1.f}, 10.f, 0xFFu};
    CHECK(!trace(memory, 0, behind));
    float inverse[12];
    const float shift[12] = {1.f, 0.f, 0.f, 5.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    CHECK(invert_affine(shift, inverse));
    CHECK(inverse[0] == 1.f && inverse[3] == -5.f);
    const float singular[12] = {};
    CHECK(!invert_affine(singular, inverse));
}

TEST(bvh_blas_matches_brute_force) {
    std::mt19937 rng(1);
    const auto scene = random_triangles(rng, 48, 4.f);
    BlasOptions plain{};
    BlasOptions half = plain;
    half.box16 = true;
    BlasOptions paired = plain;
    paired.pair = true;
    BlasOptions paired_half = paired;
    paired_half.box16 = true;
    CHECK(same_scene(build_blas(scene, plain), 0x10000, scene, rng, 200));
    CHECK(same_scene(build_blas(scene, half), 0x20000, scene, rng, 200));
    const auto grid = grid_mesh(4, 4.f);
    const auto unpaired = build_blas(grid, plain);
    const auto paired_grid = build_blas(grid, paired);
    CHECK(paired_grid.size() < unpaired.size());
    CHECK(same_scene(paired_grid, 0x30000, grid, rng, 200));
    CHECK(same_scene(build_blas(grid, paired_half), 0x40000, grid, rng, 200));
    const Triangle quad[2] = {triangle(0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f),
                              triangle(0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f)};
    const auto paired_quad = build_blas(quad, paired);
    CHECK(validate(paired_quad, true).empty());
    Memory memory;
    memory.place(0x50000, paired_quad);
    Ray edge{{0.5f, 0.5f, -1.f}, {0.f, 0.f, 1.f}, 10.f, 0xFFu};
    SceneInstance brute_scene;
    brute_scene.mask = 0xFFu;
    brute_scene.triangles.assign(quad, quad + 2);
    CHECK(close_hit(trace(memory, 0x50000, edge), brute_force(std::span<const SceneInstance>(&brute_scene, 1), edge)));
    Triangle nan_scene[2] = {quad[0], quad[1]};
    nan_scene[1].vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
    BlasOptions half_only{};
    half_only.box16 = true;
    const auto nan_build = build_blas(nan_scene, half_only);
    CHECK(validate(nan_build, true).empty());
    CHECK(box16_rounding(nan_build).empty());
}

TEST(bvh_tlas_masks_and_missing_blas) {
    std::mt19937 rng(5);
    const auto mesh_a = grid_mesh(3, 2.f);
    const auto mesh_b = random_triangles(rng, 24, 1.f);
    BlasOptions paired{};
    paired.pair = true;
    BlasOptions half{};
    half.box16 = true;
    const auto blas_a = build_blas(mesh_a, paired);
    const auto blas_b = build_blas(mesh_b, half);
    CHECK(validate(blas_a, true).empty());
    CHECK(validate(blas_b, true).empty());
    const Header header_a = header_of(blas_a);
    const Header header_b = header_of(blas_b);
    const float c = std::cos(0.7f);
    const float s = std::sin(0.7f);
    InstanceInput inputs[4]{};
    const float transforms[4][12] = {
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f},
        {c, -s, 0.f, 3.f, s, c, 0.f, -1.f, 0.f, 0.f, 1.f, 0.5f},
        {2.f, 0.f, 0.f, -3.f, 0.f, 0.5f, 0.f, 2.f, 0.f, 0.f, 1.5f, 1.f},
        {c, 0.f, s, 0.f, 0.f, 1.f, 0.f, 3.f, -s, 0.f, c, -2.f},
    };
    const std::uint32_t masks[4] = {0x01u, 0x02u, 0x01u, 0x04u};
    for (int i = 0; i < 4; ++i) {
        for (int component = 0; component < 12; ++component) inputs[i].transform[component] = transforms[i][component];
        inputs[i].blas_address = i < 2 ? 0x100000 : 0x200000;
        inputs[i].blas_root = i < 2 ? header_a.root : header_b.root;
        const Header& source = i < 2 ? header_a : header_b;
        for (int axis = 0; axis < 3; ++axis) {
            inputs[i].blas_min[axis] = source.bounds_min[axis];
            inputs[i].blas_max[axis] = source.bounds_max[axis];
        }
        inputs[i].instance_id = 10u + static_cast<std::uint32_t>(i);
        inputs[i].mask = masks[i];
    }
    InstanceInput masked = inputs[0];
    masked.mask = 0;
    masked.transform[3] = 8.f;
    InstanceInput missing = inputs[2];
    missing.blas_address = 0;
    missing.mask = 0xFFu;
    missing.transform[3] = -8.f;
    InstanceInput batch[6];
    for (int i = 0; i < 4; ++i) batch[i] = inputs[i];
    batch[4] = masked;
    batch[5] = missing;
    const auto tlas = build_tlas(batch, false);
    CHECK(validate(tlas, false).empty());
    const auto unaligned = batch[0];
    InstanceInput bad = unaligned;
    bad.blas_address = 0x12340;
    CHECK(build_tlas(std::span<const InstanceInput>(&bad, 1), false).empty());
    Memory memory;
    memory.place(0x100000, blas_a);
    memory.place(0x200000, blas_b);
    memory.place(0x300000, tlas);
    SceneInstance scene[6];
    for (int i = 0; i < 4; ++i) {
        scene[i].transform = std::array<float, 12>{};
        for (int component = 0; component < 12; ++component) (*scene[i].transform)[component] = inputs[i].transform[component];
        scene[i].mask = masks[i];
        scene[i].triangles = i < 2 ? mesh_a : mesh_b;
    }
    scene[4] = scene[0];
    scene[4].mask = 0;
    for (int component = 0; component < 12; ++component) (*scene[4].transform)[component] = masked.transform[component];
    scene[5].transform = std::array<float, 12>{};
    for (int component = 0; component < 12; ++component) (*scene[5].transform)[component] = missing.transform[component];
    scene[5].mask = 0xFFu;
    std::uniform_real_distribution<float> origin_dist(-8.f, 8.f);
    std::uniform_real_distribution<float> aim(-4.f, 4.f);
    int hits = 0;
    for (std::uint32_t mask : {0xFFu, 0x01u, 0x06u}) {
        for (int i = 0; i < 120; ++i) {
            Ray ray{};
            ray.origin[0] = origin_dist(rng);
            ray.origin[1] = origin_dist(rng);
            ray.origin[2] = origin_dist(rng);
            const float target[3] = {aim(rng), aim(rng), aim(rng)};
            ray.direction[0] = target[0] - ray.origin[0];
            ray.direction[1] = target[1] - ray.origin[1];
            ray.direction[2] = target[2] - ray.origin[2];
            ray.t_max = std::numeric_limits<float>::infinity();
            ray.mask = mask;
            const auto traced = trace(memory, 0x300000, ray);
            const auto brute = brute_force(scene, ray);
            CHECK(close_hit(traced, brute));
            if (traced) ++hits;
        }
    }
    CHECK(hits > 20);
    const auto masked_only = build_tlas(std::span<const InstanceInput>(&masked, 1), false);
    CHECK(validate(masked_only, false).empty());
    const Header masked_header = header_of(masked_only);
    float box[6];
    std::memcpy(box, masked_only.data() + pointer_offset(masked_header.root) + 16, sizeof(box));
    CHECK(box[0] > box[3] && box[1] > box[4] && box[2] > box[5]);
}

TEST(bvh_validate_reports_damage_and_survives_fuzz) {
    std::mt19937 rng(4);
    const auto scene = random_triangles(rng, 30, 3.f);
    const auto data = build_blas(scene, {});
    CHECK(validate(data, true).empty());
    CHECK(validate(std::span<const std::uint8_t>(data.data(), 10), std::nullopt) ==
          std::vector<std::string>{"buffer shorter than the 64-byte header"});
    auto longer = data;
    longer.resize(data.size() + 64);
    CHECK(!validate(longer, true).empty());
    auto count = data;
    count[12] ^= 0x1;
    CHECK(!validate(count, true).empty());
    auto dehydrated = data;
    std::uint32_t flags = 0;
    std::memcpy(&flags, dehydrated.data(), 4);
    flags |= 0x01000000u;
    std::memcpy(dehydrated.data(), &flags, 4);
    CHECK(!validate(dehydrated, std::nullopt).empty());
    const Header header = header_of(data);
    auto outside = data;
    float far[3] = {1e6f, 1e6f, 1e6f};
    std::memcpy(outside.data() + pointer_offset(header.root) + 16, far, sizeof(far));
    CHECK(!validate(outside, true).empty());
    auto bad_round = build_blas(scene, BlasOptions{0, 1, true, false});
    CHECK(box16_rounding(bad_round).empty());
    bad_round[pointer_offset(header_of(bad_round).root) + 16] ^= 1;
    CHECK(!box16_rounding(bad_round).empty());
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int i = 0; i < 2000; ++i) {
        auto mutated = data;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int flip = 0; flip < flips && !mutated.empty(); ++flip)
            mutated[rng() % mutated.size()] = static_cast<std::uint8_t>(byte_dist(rng));
        if (rng() % 5 == 0 && mutated.size() > 8) mutated.resize(rng() % mutated.size());
        if (rng() % 7 == 0) mutated.push_back(static_cast<std::uint8_t>(byte_dist(rng)));
        const auto problems = validate(mutated, std::nullopt);
        CHECK(problems.size() < 100000);
        Memory memory;
        memory.place(0x1000, mutated);
        Ray ray{{0.2f, 0.2f, -1.f}, {0.f, 0.f, 1.f}, 20.f, 0xFFu};
        const auto hit = trace(memory, 0x1000, ray);
        CHECK(!hit || std::isfinite(hit->t));
        if (i % 3 == 0) {
            const auto rounding = box16_rounding(mutated);
            CHECK(rounding.size() < 100000);
        }
    }
}
