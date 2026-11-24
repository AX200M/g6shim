// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sampler.hpp"

#include <algorithm>
#include <cstring>

namespace g6shim::d3d12_xs::sampler {

namespace {

constexpr std::uint32_t kLinearBilinear = 1;
constexpr std::uint32_t kLinearAnisotropic = 3;
constexpr std::uint32_t kMipLinear = 2;
constexpr std::uint32_t kMinimumReduction = 1;
constexpr std::uint32_t kMaximumReduction = 2;
constexpr std::uint32_t kAnisotropicFilter = 0x55;
constexpr std::uint32_t kFilterReductionShift = 7;
constexpr std::uint32_t kMinimumCompare = 2;
constexpr std::uint32_t kMaximumCompare = 7;
constexpr std::uint32_t kMaximumAnisotropy = 16;

struct Fields {
    std::uint32_t mag;
    std::uint32_t min;
    std::uint32_t mip;
    std::uint32_t mode;
    std::array<std::uint32_t, 3> address;
    std::uint32_t anisotropy;
    std::uint32_t compare;
    float min_lod;
    float max_lod;
    float bias;
    std::array<float, 4> border;
};

float word_float(const std::array<std::uint32_t, kConsoleWords>& words, std::size_t index) {
    float value = 0;
    std::memcpy(&value, &words[index], sizeof value);
    return value;
}

D3D12_SAMPLER_DESC host_desc(const Fields& fields) {
    const auto linear = [](std::uint32_t filter) {
        return filter == kLinearBilinear || filter == kLinearAnisotropic ? 1u : 0u;
    };
    const bool anisotropic = fields.mag >= 2 || fields.min >= 2;
    const bool compares = fields.compare >= kMinimumCompare && fields.compare <= kMaximumCompare;
    std::uint32_t reduction = 0;
    if (fields.mode == kMinimumReduction)
        reduction = 2;
    else if (fields.mode == kMaximumReduction)
        reduction = 3;
    else
        reduction = compares ? 1 : 0;
    const std::uint32_t filter = anisotropic
                                     ? kAnisotropicFilter
                                     : (linear(fields.min) << 4) | (linear(fields.mag) << 2) |
                                           (fields.mip == kMipLinear ? 1u : 0u);
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D12_FILTER>((reduction << kFilterReductionShift) | filter);
    desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(fields.address[0]);
    desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(fields.address[1]);
    desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(fields.address[2]);
    desc.MipLODBias = fields.bias;
    desc.MaxAnisotropy = anisotropic ? std::clamp(fields.anisotropy, 1u, kMaximumAnisotropy) : 1;
    desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(std::max(fields.compare, 1u));
    std::copy(fields.border.begin(), fields.border.end(), desc.BorderColor);
    desc.MinLOD = fields.min_lod;
    desc.MaxLOD = fields.mip == 0 ? fields.min_lod : fields.max_lod;
    return desc;
}

}  // namespace

D3D12_SAMPLER_DESC console(const std::array<std::uint32_t, kConsoleWords>& words) {
    return host_desc(Fields{
        words[1],
        words[2],
        words[3],
        words[5],
        {words[6], words[7], words[8]},
        words[13],
        words[16],
        word_float(words, 9),
        word_float(words, 10),
        word_float(words, 11),
        {word_float(words, 17), word_float(words, 18), word_float(words, 19), word_float(words, 20)},
    });
}

}  // namespace g6shim::d3d12_xs::sampler
