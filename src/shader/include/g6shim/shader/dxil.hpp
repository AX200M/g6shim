// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <utility>

namespace g6shim::shader {

struct ShaderKind {
    enum class Id : std::uint8_t {
        Pixel,
        Vertex,
        Geometry,
        Hull,
        Domain,
        Compute,
        Library,
        RayGeneration,
        Intersection,
        AnyHit,
        ClosestHit,
        Miss,
        Callable,
        Mesh,
        Amplification,
        Node,
        Unknown,
    };

    Id id = Id::Unknown;
    std::uint16_t raw = 0;

    friend bool operator==(const ShaderKind&, const ShaderKind&) = default;
};

ShaderKind shader_kind_from_raw(std::uint16_t value);

struct XboxExtras {
    bool xdat = false;
    bool xbop = false;

    friend bool operator==(const XboxExtras&, const XboxExtras&) = default;
};

struct DxilProgram {
    ShaderKind kind;
    std::pair<std::uint8_t, std::uint8_t> shader_model{std::uint8_t{0}, std::uint8_t{0}};
    std::uint32_t dxil_version = 0;
    std::span<const std::uint8_t> bitcode;
    std::span<const std::uint8_t> prefix;
    std::span<const std::uint8_t> trailer;

    static DxilProgram parse(std::span<const std::uint8_t> part);

    bool is_standard_layout() const;
    XboxExtras xbox_extras() const;
};

}  // namespace g6shim::shader
