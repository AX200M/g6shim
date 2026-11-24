// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace g6shim::d3d12_xs::bvh::reference {

struct Triangle {
    float vertices[3][3];
};

struct BlasOptions {
    std::uint32_t geometry_index = 0;
    std::uint32_t geometry_flags = 1;
    bool box16 = false;
    bool pair = false;
};

struct InstanceInput {
    float transform[12];
    std::uint64_t blas_address;
    std::uint32_t blas_root;
    float blas_min[3];
    float blas_max[3];
    std::uint32_t instance_id;
    std::uint32_t mask;
    std::uint32_t contribution;
    std::uint32_t flags;
};

std::vector<std::uint8_t> build_blas(std::span<const Triangle> triangles, const BlasOptions& options);
std::vector<std::uint8_t> build_tlas(std::span<const InstanceInput> instances, bool box16);

class Memory {
public:
    void place(std::uint64_t address, std::span<const std::uint8_t> bytes);
    bool read(std::uint64_t address, std::size_t count, void* out) const;

private:
    struct Region {
        std::uint64_t address;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Region> regions_;
};

struct Ray {
    float origin[3];
    float direction[3];
    float t_max;
    std::uint32_t mask;
};

struct Hit {
    float t;
    std::optional<std::uint32_t> instance;
    std::uint32_t geometry;
    std::uint32_t primitive;
    float barycentrics[2];
    bool front;
};

std::optional<Hit> trace(const Memory& memory, std::uint64_t structure, const Ray& ray);

struct SceneInstance {
    std::optional<std::array<float, 12>> transform;
    std::uint32_t mask;
    std::vector<Triangle> triangles;
};

std::optional<Hit> brute_force(std::span<const SceneInstance> scene, const Ray& ray);

std::vector<std::string> validate(std::span<const std::uint8_t> data, std::optional<bool> bottom_level);
std::vector<std::string> box16_rounding(std::span<const std::uint8_t> data);

bool invert_affine(const float transform[12], float out[12]);

}  // namespace g6shim::d3d12_xs::bvh::reference
