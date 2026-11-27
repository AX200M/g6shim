// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "translate.hpp"

#include <array>
#include <utility>

namespace g6shim::upscaler {

namespace {

constexpr std::size_t kMostChained = 64;

bool backend_device(StructType type) {
    const StructType backend = type & kBackendMask;
    return (backend == kBackendConsole || backend == kBackendDx12) && (type & ~kBackendMask) == kBackendDevice;
}

constexpr std::uint32_t format(SurfaceFormat value) { return static_cast<std::uint32_t>(value); }

constexpr std::array<std::pair<DXGI_FORMAT, SurfaceFormat>, 48> kFormats{{
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, SurfaceFormat::kR32G32B32A32Typeless},
    {DXGI_FORMAT_R32G32B32A32_FLOAT, SurfaceFormat::kR32G32B32A32Float},
    {DXGI_FORMAT_R32G32B32A32_UINT, SurfaceFormat::kR32G32B32A32Uint},
    {DXGI_FORMAT_R32G32B32_FLOAT, SurfaceFormat::kR32G32B32Float},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, SurfaceFormat::kR16G16B16A16Typeless},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, SurfaceFormat::kR16G16B16A16Float},
    {DXGI_FORMAT_R32G32_TYPELESS, SurfaceFormat::kR32G32Typeless},
    {DXGI_FORMAT_R32G32_FLOAT, SurfaceFormat::kR32G32Float},
    {DXGI_FORMAT_R32G32_UINT, SurfaceFormat::kR32G32Uint},
    {DXGI_FORMAT_R32G8X24_TYPELESS, SurfaceFormat::kR32Float},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, SurfaceFormat::kR32Float},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, SurfaceFormat::kR32Float},
    {DXGI_FORMAT_R24G8_TYPELESS, SurfaceFormat::kR32Uint},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, SurfaceFormat::kR32Uint},
    {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, SurfaceFormat::kR32Uint},
    {DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, SurfaceFormat::kR8Uint},
    {DXGI_FORMAT_X24_TYPELESS_G8_UINT, SurfaceFormat::kR8Uint},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, SurfaceFormat::kR10G10B10A2Typeless},
    {DXGI_FORMAT_R10G10B10A2_UNORM, SurfaceFormat::kR10G10B10A2Unorm},
    {DXGI_FORMAT_R11G11B10_FLOAT, SurfaceFormat::kR11G11B10Float},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, SurfaceFormat::kR8G8B8A8Typeless},
    {DXGI_FORMAT_R8G8B8A8_UNORM, SurfaceFormat::kR8G8B8A8Unorm},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, SurfaceFormat::kR8G8B8A8Srgb},
    {DXGI_FORMAT_R8G8B8A8_SNORM, SurfaceFormat::kR8G8B8A8Snorm},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, SurfaceFormat::kB8G8R8A8Typeless},
    {DXGI_FORMAT_B8G8R8A8_UNORM, SurfaceFormat::kB8G8R8A8Unorm},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, SurfaceFormat::kB8G8R8A8Srgb},
    {DXGI_FORMAT_R16G16_TYPELESS, SurfaceFormat::kR16G16Typeless},
    {DXGI_FORMAT_R16G16_FLOAT, SurfaceFormat::kR16G16Float},
    {DXGI_FORMAT_R16G16_UINT, SurfaceFormat::kR16G16Uint},
    {DXGI_FORMAT_R16G16_SINT, SurfaceFormat::kR16G16Sint},
    {DXGI_FORMAT_R32_TYPELESS, SurfaceFormat::kR32Typeless},
    {DXGI_FORMAT_D32_FLOAT, SurfaceFormat::kR32Float},
    {DXGI_FORMAT_R32_FLOAT, SurfaceFormat::kR32Float},
    {DXGI_FORMAT_R32_UINT, SurfaceFormat::kR32Uint},
    {DXGI_FORMAT_R8G8_TYPELESS, SurfaceFormat::kR8G8Typeless},
    {DXGI_FORMAT_R8G8_UNORM, SurfaceFormat::kR8G8Unorm},
    {DXGI_FORMAT_R8G8_UINT, SurfaceFormat::kR8G8Uint},
    {DXGI_FORMAT_R16_TYPELESS, SurfaceFormat::kR16Typeless},
    {DXGI_FORMAT_R16_FLOAT, SurfaceFormat::kR16Float},
    {DXGI_FORMAT_R16_UINT, SurfaceFormat::kR16Uint},
    {DXGI_FORMAT_D16_UNORM, SurfaceFormat::kR16Unorm},
    {DXGI_FORMAT_R16_UNORM, SurfaceFormat::kR16Unorm},
    {DXGI_FORMAT_R16_SNORM, SurfaceFormat::kR16Snorm},
    {DXGI_FORMAT_R8_TYPELESS, SurfaceFormat::kR8Typeless},
    {DXGI_FORMAT_R8_UNORM, SurfaceFormat::kR8Unorm},
    {DXGI_FORMAT_R8_SNORM, SurfaceFormat::kR8Snorm},
    {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, SurfaceFormat::kR9G9B9E5SharedExp},
}};

constexpr std::array kTypeless{
    SurfaceFormat::kR32G32B32A32Typeless, SurfaceFormat::kR16G16B16A16Typeless, SurfaceFormat::kR32G32Typeless,
    SurfaceFormat::kR10G10B10A2Typeless,  SurfaceFormat::kR8G8B8A8Typeless,     SurfaceFormat::kB8G8R8A8Typeless,
    SurfaceFormat::kR16G16Typeless,       SurfaceFormat::kR32Typeless,          SurfaceFormat::kR8G8Typeless,
    SurfaceFormat::kR16Typeless,          SurfaceFormat::kR8Typeless,
};

bool typeless(std::uint32_t value) {
    for (const SurfaceFormat candidate : kTypeless)
        if (format(candidate) == value) return true;
    return false;
}

}  // namespace

Header* CreateChain::head(bool with_version) {
    if (!has_upscale) return nullptr;
    Header* tail = &backend.header;
    tail->next = nullptr;
    if (with_version && has_version) {
        version.header.next = tail;
        tail = &version.header;
    }
    upscale.header.next = tail;
    return &upscale.header;
}

CreateChain translate_create(const Header* desc, void* host_device) {
    CreateChain chain;
    chain.backend.header.type = kBackendDx12 | kBackendDevice;
    chain.backend.device = host_device;
    std::size_t walked = 0;
    for (const Header* at = desc; at != nullptr && walked < kMostChained; at = at->next, ++walked) {
        if (at->type == kCreateUpscale) {
            chain.upscale = *reinterpret_cast<const CreateUpscale*>(at);
            chain.has_upscale = true;
        } else if (at->type == kCreateUpscaleVersion) {
            chain.version = *reinterpret_cast<const CreateUpscaleVersion*>(at);
            chain.has_version = true;
        } else if (!backend_device(at->type)) {
            chain.dropped.push_back(at->type);
        }
    }
    return chain;
}

std::vector<StructType> chained_types(const Header* desc) {
    std::vector<StructType> types;
    for (const Header* at = desc; at != nullptr && types.size() < kMostChained; at = at->next) types.push_back(at->type);
    return types;
}

std::uint32_t surface_format(DXGI_FORMAT dxgi) {
    for (const auto& [from, to] : kFormats)
        if (from == dxgi) return format(to);
    return format(SurfaceFormat::kUnknown);
}

std::optional<std::uint32_t> resource_state(std::uint32_t d3d_state) {
    if (d3d_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        return static_cast<std::uint32_t>(State::kComputeRead);
    if (d3d_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return static_cast<std::uint32_t>(State::kUnorderedAccess);
    return std::nullopt;
}

ResourceDescription describe(const D3D12_RESOURCE_DESC& host, const ResourceDescription& title) {
    ResourceDescription out = title;
    switch (host.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            out.type = static_cast<std::uint32_t>(ResourceType::kTexture1D);
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            out.type = static_cast<std::uint32_t>(ResourceType::kTexture3D);
            break;
        case D3D12_RESOURCE_DIMENSION_BUFFER:
            out.type = static_cast<std::uint32_t>(ResourceType::kBuffer);
            break;
        default:
            out.type = static_cast<std::uint32_t>(host.DepthOrArraySize == 6 ? ResourceType::kTextureCube
                                                                             : ResourceType::kTexture2D);
            break;
    }
    out.width = static_cast<std::uint32_t>(host.Width);
    out.height = host.Height;
    out.depth = host.DepthOrArraySize;
    out.mip_count = host.MipLevels;
    const std::uint32_t hosted = surface_format(host.Format);
    const bool title_typed = title.format != format(SurfaceFormat::kUnknown) && !typeless(title.format);
    if (!typeless(hosted) || !title_typed) out.format = hosted;
    if ((host.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0)
        out.usage |= kUsageUnorderedAccess;
    else
        out.usage &= ~kUsageUnorderedAccess;
    return out;
}

}  // namespace g6shim::upscaler
