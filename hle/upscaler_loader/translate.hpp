// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api.hpp"

#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace g6shim::upscaler {

struct CreateChain {
    CreateUpscale upscale{};
    CreateUpscaleVersion version{};
    BackendDevice backend{};
    bool has_upscale = false;
    bool has_version = false;
    std::vector<StructType> dropped;

    Header* head(bool with_version);
};

CreateChain translate_create(const Header* desc, void* host_device);
std::vector<StructType> chained_types(const Header* desc);

std::uint32_t surface_format(DXGI_FORMAT format);
std::optional<std::uint32_t> resource_state(std::uint32_t d3d_state);
ResourceDescription describe(const D3D12_RESOURCE_DESC& host, const ResourceDescription& title);

}  // namespace g6shim::upscaler
