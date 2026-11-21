// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g6shim::acphal {

inline constexpr std::size_t kShapeKinds = 6;
inline constexpr std::size_t kShapeKindSrc = 0;
inline constexpr std::size_t kShapeKindEqComp = 1;
inline constexpr std::size_t kShapeKindFilterVolume = 2;
inline constexpr std::size_t kShapeKindDma = 3;
inline constexpr std::size_t kShapeKindXma = 4;
inline constexpr std::size_t kShapeKindPcm = 5;

inline constexpr std::uint32_t kSrcContextBytes = 32;
inline constexpr std::uint32_t kEqCompContextBytes = 160;
inline constexpr std::uint32_t kFilterVolumeContextBytes = 32;
inline constexpr std::uint32_t kDmaContextBytes = 16;
inline constexpr std::uint32_t kXmaContextBytes = 64;
inline constexpr std::uint32_t kPcmContextBytes = 32;
inline constexpr std::array<std::uint32_t, kShapeKinds> kContextBytes = {
    kSrcContextBytes, kEqCompContextBytes, kFilterVolumeContextBytes,
    kDmaContextBytes, kXmaContextBytes,    kPcmContextBytes};
inline constexpr std::array<std::uint32_t, kShapeKinds> kArrayAlignment = {0x20, 0x10, 0x20, 0x10, 0x40, 0x40};
inline constexpr std::array<std::uint32_t, kShapeKinds> kMaxContexts = {0x1000, 0x1000, 0x1000, 0x1000, 0x200, 0xE00};
inline constexpr std::array<bool, kShapeKinds> kKindProcessed = {true, false, false, true, true, false};
inline constexpr std::array<const char*, kShapeKinds> kKindNames = {"SRC", "EQ/compressor", "filter/volume",
                                                                     "DMA", "XMA",           "PCM"};

inline constexpr std::size_t kConfigCountsOffset = 0x00;
inline constexpr std::size_t kConfigArraysOffset = 0x18;
inline constexpr std::size_t kConfigApuAddressesOffset = 0x48;
inline constexpr std::size_t kConfigBytes = 0x60;

inline constexpr HRESULT kShapeContextsInUse = static_cast<HRESULT>(0x8AC80001);
inline constexpr HRESULT kShapeContextsNotAllocated = static_cast<HRESULT>(0x8AC80002);

using ShapeCounts = std::array<std::uint32_t, kShapeKinds>;
using ShapeArrays = std::array<void*, kShapeKinds>;
using ShapeApuAddresses = std::array<std::uint32_t, kShapeKinds>;

inline ShapeCounts read_shape_counts(const void* config) {
    ShapeCounts counts{};
    std::memcpy(counts.data(), static_cast<const unsigned char*>(config) + kConfigCountsOffset, sizeof(counts));
    return counts;
}

inline void write_shape_arrays(void* config, const ShapeArrays& arrays, const ShapeApuAddresses& apu) {
    auto* bytes = static_cast<unsigned char*>(config);
    std::memcpy(bytes + kConfigArraysOffset, arrays.data(), sizeof(arrays));
    std::memcpy(bytes + kConfigApuAddressesOffset, apu.data(), sizeof(apu));
}

inline bool shape_counts_valid(const ShapeCounts& counts) {
    for (std::size_t kind = 0; kind < kShapeKinds; ++kind)
        if (counts[kind] > kMaxContexts[kind]) return false;
    const bool converters_used = counts[kShapeKindSrc] || counts[kShapeKindPcm];
    return !converters_used || counts[kShapeKindSrc] == counts[kShapeKindXma] + counts[kShapeKindPcm];
}

inline std::size_t shape_array_bytes(std::size_t kind, std::uint32_t count) {
    return std::size_t(kContextBytes[kind]) * count;
}

}  // namespace g6shim::acphal
