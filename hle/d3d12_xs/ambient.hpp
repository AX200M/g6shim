// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "host.hpp"

#include <windows.h>

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::d3d12_xs::ambient {

enum class Role : std::uint8_t { kResolve, kRadiance, kAir };

struct Place {
    std::uint8_t slot = 0;
    std::uint32_t offset_dwords = 0;
};

struct RoleSetting {
    Role role = Role::kResolve;
    std::string name;
    Place image;
    std::optional<Place> depth;
};

enum class Mode : std::uint8_t { kOff, kEstimate, kConstant };

struct Settings {
    Mode mode = Mode::kOff;
    float level = 0;
    std::array<float, 2> shade = {1.0f, 0.6f};
    float settle_ms = 250.0f;
    float jump = 3.0f;
    float fresh = 0.0f;
    float air_cap = 0.0f;
    float near_plane = 0.1f;
    float tan_half_y = 0.45f;
    std::vector<RoleSetting> roles;
};

Settings parse_settings(std::string_view level, std::string_view roles_text);
const Settings& settings();
bool on();
std::optional<Role> role_of(std::span<const std::uint8_t> container);
const RoleSetting* setting(Role role);

struct Views {
    D3D12_GPU_DESCRIPTOR_HANDLE image{};
    D3D12_GPU_DESCRIPTOR_HANDLE depth{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slices = 1;
};

bool image_fits(Role role, const D3D12_RESOURCE_DESC& image, const D3D12_RESOURCE_DESC* depth);
bool after(host::HostList& list, Role role, const Views& views);
bool record(host::HostList& list, Role role, const Views& views, const Settings& set);
bool passes_ready();

}  // namespace g6shim::d3d12_xs::ambient
