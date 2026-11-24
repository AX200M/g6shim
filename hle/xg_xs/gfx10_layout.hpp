// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace g6shim::gfx10 {

inline constexpr std::uint32_t kSwizzleLinear = 0;
inline constexpr std::uint32_t kSwizzle256BStandard = 1;
inline constexpr std::uint32_t kSwizzle256BDisplay = 2;
inline constexpr std::uint32_t kSwizzle4KBStandard = 5;
inline constexpr std::uint32_t kSwizzle4KBDisplay = 6;
inline constexpr std::uint32_t kSwizzle64KBStandard = 9;
inline constexpr std::uint32_t kSwizzle64KBDisplay = 10;
inline constexpr std::uint32_t kSwizzle64KBStandardTiled = 17;
inline constexpr std::uint32_t kSwizzle64KBDisplayTiled = 18;
inline constexpr std::uint32_t kSwizzle4KBStandardXor = 21;
inline constexpr std::uint32_t kSwizzle4KBDisplayXor = 22;
inline constexpr std::uint32_t kSwizzle4KBRenderXor = 23;
inline constexpr std::uint32_t kSwizzle64KBZOrderXor = 24;
inline constexpr std::uint32_t kSwizzle64KBStandardXor = 25;
inline constexpr std::uint32_t kSwizzle64KBDisplayXor = 26;
inline constexpr std::uint32_t kSwizzle64KBRenderXor = 27;

inline constexpr std::uint32_t kMaxExtent = 1u << 14;
inline constexpr std::uint32_t kMaxMipLevels = 16;

constexpr bool is_standard(std::uint32_t swizzle) {
    return swizzle == kSwizzle256BStandard || swizzle == kSwizzle4KBStandard ||
           swizzle == kSwizzle64KBStandard || swizzle == kSwizzle64KBStandardTiled ||
           swizzle == kSwizzle4KBStandardXor || swizzle == kSwizzle64KBStandardXor;
}

constexpr bool is_display(std::uint32_t swizzle) {
    return swizzle == kSwizzle256BDisplay || swizzle == kSwizzle4KBDisplay ||
           swizzle == kSwizzle64KBDisplay || swizzle == kSwizzle64KBDisplayTiled ||
           swizzle == kSwizzle4KBDisplayXor || swizzle == kSwizzle64KBDisplayXor;
}

constexpr bool is_z_order(std::uint32_t swizzle) { return swizzle == kSwizzle64KBZOrderXor; }
constexpr bool is_xor(std::uint32_t swizzle) { return swizzle >= 16; }

constexpr std::optional<std::uint32_t> block_log2(std::uint32_t swizzle) {
    switch (swizzle) {
    case kSwizzleLinear: return 0;
    case kSwizzle256BStandard:
    case kSwizzle256BDisplay: return 8;
    case kSwizzle4KBStandard:
    case kSwizzle4KBDisplay:
    case kSwizzle4KBStandardXor:
    case kSwizzle4KBDisplayXor:
    case kSwizzle4KBRenderXor: return 12;
    case kSwizzle64KBStandard:
    case kSwizzle64KBDisplay:
    case kSwizzle64KBStandardTiled:
    case kSwizzle64KBDisplayTiled:
    case kSwizzle64KBZOrderXor:
    case kSwizzle64KBStandardXor:
    case kSwizzle64KBDisplayXor:
    case kSwizzle64KBRenderXor: return 16;
    default: return std::nullopt;
    }
}

enum class Dimension { kTex1d, kTex2d, kTex3d };
enum class Error { kOk, kInvalid, kUnsupported, kTooSmall };

inline constexpr std::uint32_t kDefaultPipes = 16;
inline constexpr std::uint32_t kDefaultPackers = 8;
inline constexpr std::uint32_t kDefaultPipeInterleave = 256;
inline constexpr std::uint32_t kDefaultCompressedFragments = 8;

struct AddressConfig {
    std::uint32_t pipes = kDefaultPipes;
    std::uint32_t packers = kDefaultPackers;
    std::uint32_t pipe_interleave = kDefaultPipeInterleave;
    std::uint32_t max_compressed_fragments = kDefaultCompressedFragments;
};

struct Desc {
    Dimension dimension = Dimension::kTex2d;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t mips = 0;
    std::uint32_t element_bytes = 0;
    std::uint32_t block = 0;
    std::uint32_t samples = 0;
    std::uint32_t swizzle = kSwizzleLinear;
    AddressConfig address{};
};

struct Tail {
    std::uint32_t offset = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

struct Mip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t pitch = 0;
    std::uint32_t rows = 0;
    std::uint32_t depth_pitch = 0;
    std::uint64_t offset = 0;
    bool in_tail = false;
    Tail tail{};
};

struct OffsetResult {
    Error error = Error::kInvalid;
    std::uint64_t offset = 0;
};

struct XorMasks {
    std::uint8_t bits = 0;
    std::uint16_t mask[4][16]{};
};

struct Layout {
    Desc desc{};
    std::array<std::uint32_t, 3> block{1, 1, 1};
    bool thick = false;
    std::uint32_t slices = 0;
    std::uint64_t slice_size = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
    std::uint32_t first_tail_mip = 0;
    std::vector<Mip> mips;
    std::array<std::vector<std::uint32_t>, 3> pattern;
    XorMasks xor_masks{};

    static std::optional<Layout> create(const Desc& desc);
    bool addressable() const;
    OffsetResult offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                        std::uint32_t sample = 0) const;
    std::uint32_t subresource_depth(std::uint32_t mip) const;
    Error detile(std::span<const std::uint8_t> tiled, std::uint32_t mip, std::uint32_t slice,
                 std::span<std::uint8_t> linear, std::size_t row_pitch, std::size_t depth_pitch) const;
    Error tile(std::span<std::uint8_t> tiled, std::uint32_t mip, std::uint32_t slice,
               std::span<const std::uint8_t> linear, std::size_t row_pitch, std::size_t depth_pitch) const;

private:
    static Layout linear(const Desc& desc);
    static Layout tiled(const Desc& desc, std::uint32_t block_log2_value);
    std::uint64_t row(const Mip& mip, std::uint32_t y, std::uint32_t z) const;
    std::uint64_t column(const Mip& mip, std::uint32_t x) const;
    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t z) const;

    template <typename Copy>
    Error walk(std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::size_t linear_len,
               std::size_t row_pitch, std::size_t depth_pitch, Copy copy) const;
};

struct Format {
    std::uint32_t bytes = 0;
    std::uint32_t block = 0;
    bool stencil = false;
};

std::optional<Format> format(std::uint32_t dxgi);

struct TextureDesc {
    Dimension dimension = Dimension::kTex2d;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth_or_array = 0;
    std::uint32_t mips = 0;
    std::uint32_t format = 0;
    std::uint32_t samples = 0;
    std::uint32_t swizzle = kSwizzleLinear;
    AddressConfig address{};
};

struct Plane {
    std::uint64_t offset = 0;
    bool stencil = false;
    Layout layout;
};

struct Texture {
    std::vector<Plane> planes;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;

    static std::optional<Texture> create(const TextureDesc& desc);
};

template <typename Copy>
Error Layout::walk(std::size_t tiled_len, std::uint32_t mip, std::uint32_t slice, std::size_t linear_len,
                   std::size_t row_pitch, std::size_t depth_pitch, Copy copy) const {
    if (mip >= mips.size()) return Error::kInvalid;
    const Mip& level = mips[mip];
    std::uint32_t first = slice;
    if (desc.dimension == Dimension::kTex3d) {
        if (slice != 0) return Error::kInvalid;
        first = 0;
    }
    if (first >= slices) return Error::kInvalid;
    if (!addressable()) return Error::kUnsupported;

    const std::size_t element_bytes = desc.element_bytes;
    const std::size_t group = element_bytes * desc.samples;
    const std::size_t bytes = static_cast<std::size_t>(level.width) * group;
    std::size_t need = bytes;
    if (level.depth > 1) {
        const std::size_t steps = static_cast<std::size_t>(level.depth) - 1;
        if (depth_pitch > (std::numeric_limits<std::size_t>::max() - need) / steps) return Error::kTooSmall;
        need += steps * depth_pitch;
    }
    if (level.height > 1) {
        const std::size_t steps = static_cast<std::size_t>(level.height) - 1;
        if (row_pitch > (std::numeric_limits<std::size_t>::max() - need) / steps) return Error::kTooSmall;
        need += steps * row_pitch;
    }
    if (tiled_len < static_cast<std::size_t>(size) || linear_len < need || row_pitch < bytes)
        return Error::kTooSmall;

    if (xor_masks.bits != 0) {
        for (std::uint32_t z = 0; z < level.depth; ++z) {
            for (std::uint32_t y = 0; y < level.height; ++y) {
                const std::size_t linear_row =
                    static_cast<std::size_t>(z) * depth_pitch + static_cast<std::size_t>(y) * row_pitch;
                for (std::uint32_t x = 0; x < level.width; ++x) {
                    for (std::uint32_t sample = 0; sample < desc.samples; ++sample) {
                        const OffsetResult at = offset(mip, x, y, first + z, sample);
                        if (at.error != Error::kOk) return at.error;
                        const std::size_t linear_at =
                            linear_row + (static_cast<std::size_t>(x) * desc.samples + sample) * element_bytes;
                        copy(static_cast<std::size_t>(at.offset), linear_at, element_bytes);
                    }
                }
            }
        }
        return Error::kOk;
    }

    struct Run {
        std::size_t tiled;
        std::size_t linear;
        std::size_t length;
    };
    const bool direct = desc.swizzle == kSwizzleLinear;
    std::vector<Run> runs;
    if (!direct) {
        std::size_t first_linear = 0;
        std::size_t first_tiled = static_cast<std::size_t>(column(level, 0));
        for (std::uint32_t x = 1; x < level.width; ++x) {
            const std::size_t tiled_offset = static_cast<std::size_t>(column(level, x));
            const std::size_t at = static_cast<std::size_t>(x) * element_bytes;
            if (tiled_offset != first_tiled + (at - first_linear)) {
                runs.push_back({first_tiled, first_linear, at - first_linear});
                first_tiled = tiled_offset;
                first_linear = at;
            }
        }
        runs.push_back({first_tiled, first_linear, bytes - first_linear});
    }

    for (std::uint32_t z = 0; z < level.depth; ++z) {
        const auto [target_slice, inner] = split(first + z);
        const std::uint64_t base = static_cast<std::uint64_t>(target_slice) * slice_size + level.offset;
        for (std::uint32_t y = 0; y < level.height; ++y) {
            const std::size_t linear_row =
                static_cast<std::size_t>(z) * depth_pitch + static_cast<std::size_t>(y) * row_pitch;
            if (direct) {
                const std::uint64_t tiled_row =
                    base + static_cast<std::uint64_t>(y) * level.pitch * static_cast<std::uint64_t>(element_bytes);
                copy(static_cast<std::size_t>(tiled_row), linear_row, bytes);
            } else {
                const std::size_t tiled_row = static_cast<std::size_t>(base + row(level, y, inner));
                for (const Run& run : runs) copy(tiled_row + run.tiled, linear_row + run.linear, run.length);
            }
        }
    }
    return Error::kOk;
}

}  // namespace g6shim::gfx10
