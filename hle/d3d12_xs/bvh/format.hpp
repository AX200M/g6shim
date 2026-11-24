// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g6shim::d3d12_xs::bvh {

inline constexpr std::uint32_t kUnit = 64;
inline constexpr std::uint32_t kHeaderSize = 64;
inline constexpr std::uint32_t kBox16Size = 64;
inline constexpr std::uint32_t kBox32Size = 128;
inline constexpr std::uint32_t kLeafSize = 64;
inline constexpr std::uint32_t kInstanceSize = 128;
inline constexpr std::uint32_t kStructureAlignment = 256;

inline constexpr std::uint32_t kTri0 = 0;
inline constexpr std::uint32_t kTri1 = 1;
inline constexpr std::uint32_t kTri2 = 2;
inline constexpr std::uint32_t kTri3 = 3;
inline constexpr std::uint32_t kBox16 = 4;
inline constexpr std::uint32_t kBox32 = 5;
inline constexpr std::uint32_t kInstance = 6;
inline constexpr std::uint32_t kAabb = 7;
inline constexpr std::uint32_t kPointerTypeMask = 7;
inline constexpr std::uint32_t kPointerUnitShift = 3;
inline constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

inline constexpr std::uint32_t kFlagBottomLevel = 0x80000000u;
inline constexpr std::uint32_t kVersion2403 = 6;
inline constexpr std::uint32_t kInstanceIdentityRotation = 0x80000000u;
inline constexpr std::uint32_t kInstanceScaledLocalSpace = 0x40000000u;
inline constexpr std::uint32_t kInstanceFlagsMask = kInstanceIdentityRotation | kInstanceScaledLocalSpace;
inline constexpr std::uint32_t kRemapIdentity = 1u | (2u << 2);

inline constexpr std::uint32_t kRadixTile = 256;
inline constexpr std::uint32_t kRadixBins = 16;
inline constexpr std::uint32_t kRadixPasses = 8;
inline constexpr std::uint32_t kSmallSortMax = 1024;

struct Header {
    std::uint32_t flags;
    std::uint32_t total_size_in_64b_nodes;
    std::uint32_t crc32;
    std::uint32_t element_count;
    float bounds_min[3];
    float bounds_max[3];
    std::uint32_t nodes_offset;
    std::uint32_t prim_remap_offset;
    std::uint32_t root;
    std::uint32_t elements_offset;
    std::uint32_t parents_offset;
    std::uint32_t compression;
};
static_assert(sizeof(Header) == kHeaderSize);

struct Leaf {
    float vertices[4][3];
    std::uint32_t geometry;
    std::uint32_t primitive0;
    std::uint32_t primitive1;
    std::uint32_t remap;
};
static_assert(sizeof(Leaf) == kLeafSize);

struct InstanceRecord {
    float object_to_world[12];
    std::uint32_t id_and_mask;
    std::uint32_t contribution_and_flags;
    std::uint32_t root_and_flags;
    std::uint32_t blas_address_256;
    float world_to_object[12];
    std::uint32_t index;
    std::uint32_t parent;
    std::uint32_t reserved[2];
};
static_assert(sizeof(InstanceRecord) == kInstanceSize);

struct Box32Node {
    std::uint32_t children[4];
    float boxes[4][6];
    std::uint32_t padding[4];
};
static_assert(sizeof(Box32Node) == kBox32Size);

struct Box16Node {
    std::uint32_t children[4];
    std::uint32_t boxes[4][3];
};
static_assert(sizeof(Box16Node) == kBox16Size);

struct Layout {
    bool top_level;
    bool box16;
    std::uint32_t node_size;
    std::uint32_t elements;
    std::uint32_t element_size;
    std::uint32_t nodes;
    std::uint32_t parents_offset;
    std::uint32_t elements_offset;
    std::uint32_t nodes_offset;
    std::uint32_t size;
    std::uint32_t parent_entries;
    std::uint32_t sort_size;
    std::uint64_t scratch;
};

bool compute_layout(std::uint32_t count, bool top_level, bool box16, Layout* out);
std::uint32_t radix_tiles(const Layout& layout);
std::uint32_t inner_nodes(const Layout& layout);
std::uint32_t kept_tiles(const Layout& layout);

constexpr std::uint32_t pointer(std::uint32_t offset, std::uint32_t type) {
    return (offset / kUnit) << kPointerUnitShift | type;
}
constexpr std::uint32_t pointer_offset(std::uint32_t p) { return (p >> kPointerUnitShift) * kUnit; }
constexpr std::uint32_t pointer_type(std::uint32_t p) { return p & kPointerTypeMask; }

std::uint16_t half_directed(float value, bool up);
float half_to_float(std::uint16_t bits);
void encode_box16(const float lo[3], const float hi[3], std::uint32_t out[3]);
void decode_box16(const std::uint32_t words[3], float lo[3], float hi[3]);

}  // namespace g6shim::d3d12_xs::bvh
