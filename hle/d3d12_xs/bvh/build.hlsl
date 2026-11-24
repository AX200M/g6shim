// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

static const uint kModeInit = 0;
static const uint kModeTriangles = 1;
static const uint kModeInstances = 2;
static const uint kModeMorton = 3;
static const uint kModeKarras = 4;
static const uint kModeNodes = 5;
static const uint kModeHeader = 6;
static const uint kModeRadixCount = 7;
static const uint kModeRadixScan = 8;
static const uint kModeRadixScatter = 9;
static const uint kModeSmallSort = 10;
static const uint kModeCopy = 12;
static const uint kModePostbuild = 13;
static const uint kModeKept = 14;
static const uint kModeKeptScan = 15;
static const uint kModeKeptIndex = 16;
static const uint kModePropagate = 17;
static const uint kModeClear = 18;

static const uint kSmallSortMax = 1024;
static const uint kRadixBits = 4;
static const uint kRadixBins = 16;
static const uint kRadixTile = 256;
static const uint kGroup = 64;
static const uint kInvalid = 0xffffffffu;
static const uint kTri0 = 0;
static const uint kTri1 = 1;
static const uint kBox16 = 4;
static const uint kBox32 = 5;
static const uint kInstance = 6;
static const uint kLeafBit = 0x80000000u;
static const uint kRemapIdentity = 0x09u;
static const uint kFlagTlas = 1;
static const uint kFlagBox16 = 2;
static const uint kFlagPair = 4;
static const uint kFlagBottomLevel = 0x80000000u;
static const uint kVersion2403 = 6;
static const uint kIdentityRotation = 0x80000000u;
static const uint kInstanceFlagsMask = 0xc0000000u;
static const uint kInfBits = 0x7f800000u;
static const uint kFloatSign = 0x80000000u;
static const uint kFloatMagnitude = 0x7fffffffu;
static const uint kFloatMantissa = 0x7fffffu;
static const uint kHalfSign = 0x8000u;
static const uint kHalfNan = 0x7e00u;
static const uint kHalfInf = 0x7c00u;
static const uint kHalfMax = 0x7bffu;
static const uint kHalfNormal = 0x0400u;
static const uint kDroppedMantissa = 0x1fffu;
static const uint kMortonEmpty = 0x3fffffffu;
static const uint kScratchKeys = 64;
static const uint kScratchNodeCount = 24;
static const uint kHeaderDwords = 16;
static const uint kQuietNan = 0x7fc00000u;
static const int kFloatBias = 127;
static const int kHalfBias = 15;
static const int kHalfLargestExponent = 16;
static const int kHalfSmallestExponent = -14;
static const uint kDepthGuard = 128;

cbuffer PushConstants : register(b0) {
    uint2 dst_va : packoffset(c0);
    uint2 scratch_va : packoffset(c0.z);
    uint2 src0_va : packoffset(c1);
    uint2 src1_va : packoffset(c1.z);
    uint2 src2_va : packoffset(c2);
    uint2 bias : packoffset(c2.z);
    uint mode : packoffset(c3);
    uint count : packoffset(c3.y);
    uint first : packoffset(c3.z);
    uint arg0 : packoffset(c3.w);
    uint arg1 : packoffset(c4);
    uint arg2 : packoffset(c4.y);
    uint geometry : packoffset(c4.z);
    uint elements : packoffset(c4.w);
    uint sort_size : packoffset(c5);
    uint elements_off : packoffset(c5.y);
    uint nodes_off : packoffset(c5.z);
    uint parents_off : packoffset(c5.w);
    uint total_size : packoffset(c6);
    uint flags : packoffset(c6.y);
};

globallycoherent RWByteAddressBuffer g_dst : register(u0);
globallycoherent RWByteAddressBuffer g_scratch : register(u1);
RWByteAddressBuffer g_src0 : register(u2);
RWByteAddressBuffer g_src1 : register(u3);
RWByteAddressBuffer g_src2 : register(u4);
RWByteAddressBuffer g_window : register(u5);

groupshared uint small_keys[kSmallSortMax];
groupshared uint small_vals[kSmallSortMax];
groupshared uint radix_hist[kRadixBins];
groupshared uint radix_rank[kRadixBins][kGroup];
groupshared uint radix_base[kRadixBins];
groupshared uint radix_part[kGroup];
groupshared uint kept_count;
groupshared uint kept_scan[kGroup];

uint uadd_carry(uint a, uint b, out uint carry) {
    uint sum = a + b;
    carry = sum < a ? 1u : 0u;
    return sum;
}

bool is_null(uint2 a) { return a.x == 0u && a.y == 0u; }

uint radix_tiles() { return (sort_size + kRadixTile - 1u) / kRadixTile; }
uint keys_off() { return kScratchKeys; }
uint vals_off() { return kScratchKeys + 4u * sort_size; }
uint keys2_off() { return kScratchKeys + 8u * sort_size; }
uint vals2_off() { return kScratchKeys + 12u * sort_size; }
uint counts_off() { return kScratchKeys + 16u * sort_size; }
uint offsets_off() { return counts_off() + 4u * kRadixBins * radix_tiles(); }
uint boxes_off() { return offsets_off() + 4u * kRadixBins * radix_tiles(); }
uint accum_off() { return boxes_off() + 24u * elements; }
uint inner_count() { return elements > 1u ? elements - 1u : 0u; }
uint left_off() { return accum_off() + 32u * arg0; }
uint right_off() { return left_off() + 4u * inner_count(); }
uint bparent_off() { return right_off() + 4u * inner_count(); }
uint kept_off() { return bparent_off() + 4u * inner_count(); }
uint kmap_off() { return kept_off() + 4u * inner_count(); }
uint kept_tile_count() { return (inner_count() + kRadixTile - 1u) / kRadixTile; }
uint ktile_counts_off() { return kmap_off() + 4u * inner_count(); }
uint ktile_offsets_off() { return ktile_counts_off() + 4u * kept_tile_count(); }
uint tptr_off() { return ktile_offsets_off() + 4u * kept_tile_count(); }

uint ordered(float f) {
    uint bits = asuint(f);
    return (bits & kFloatSign) != 0u ? ~bits : (bits | kFloatSign);
}

float unordered(uint bits) {
    return asfloat((bits & kFloatSign) != 0u ? (bits & kFloatMagnitude) : ~bits);
}

void store_box(uint index, float3 lo, float3 hi) {
    uint offset = boxes_off() + 24u * index;
    g_scratch.Store(offset, asuint(lo.x));
    g_scratch.Store(offset + 4u, asuint(lo.y));
    g_scratch.Store(offset + 8u, asuint(lo.z));
    g_scratch.Store(offset + 12u, asuint(hi.x));
    g_scratch.Store(offset + 16u, asuint(hi.y));
    g_scratch.Store(offset + 20u, asuint(hi.z));
}

void load_box(uint index, out float3 lo, out float3 hi) {
    uint offset = boxes_off() + 24u * index;
    lo = float3(asfloat(g_scratch.Load(offset)), asfloat(g_scratch.Load(offset + 4u)), asfloat(g_scratch.Load(offset + 8u)));
    hi = float3(asfloat(g_scratch.Load(offset + 12u)), asfloat(g_scratch.Load(offset + 16u)), asfloat(g_scratch.Load(offset + 20u)));
}

bool box_empty(float3 lo, float3 hi) { return !(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z); }

void empty_box(out float3 lo, out float3 hi) {
    lo = float3(asfloat(kInfBits), asfloat(kInfBits), asfloat(kInfBits));
    hi = -lo;
}

void grow_centroid_bounds(float3 lo, float3 hi) {
    if (box_empty(lo, hi)) return;
    float3 centre = (lo + hi) * 0.5;
    uint ignored;
    g_scratch.InterlockedMin(0u, ordered(centre.x), ignored);
    g_scratch.InterlockedMin(4u, ordered(centre.y), ignored);
    g_scratch.InterlockedMin(8u, ordered(centre.z), ignored);
    g_scratch.InterlockedMax(12u, ordered(centre.x), ignored);
    g_scratch.InterlockedMax(16u, ordered(centre.y), ignored);
    g_scratch.InterlockedMax(20u, ordered(centre.z), ignored);
}

bool is_tlas() { return (flags & kFlagTlas) != 0u; }
bool is_box16() { return (flags & kFlagBox16) != 0u; }
uint element_size() { return is_tlas() ? 128u : 64u; }
uint node_size() { return is_box16() ? 64u : 128u; }

uint element_ptr(uint element) {
    return ((elements_off + element * element_size()) >> 6) << 3 | (is_tlas() ? kInstance : kTri0);
}

uint node_ptr(uint node) {
    return ((nodes_off + node * node_size()) >> 6) << 3 | (is_box16() ? kBox16 : kBox32);
}

uint ptr_unit(uint pointer) { return pointer >> 3; }
bool ptr_is_node(uint pointer) { return (pointer & 7u) == kBox16 || (pointer & 7u) == kBox32; }
bool ptr_box16(uint pointer) { return (pointer & 7u) == kBox16; }

uint half_directed(float value, bool up) {
    uint bits = asuint(value);
    uint sign = (bits >> 16) & kHalfSign;
    uint magnitude = bits & kFloatMagnitude;
    bool away = up != (sign != 0u);
    if (magnitude > kInfBits) return sign | kHalfNan;
    if (magnitude == 0u) return sign;
    int exponent = int(magnitude >> 23) - kFloatBias;
    if (exponent >= kHalfLargestExponent) return sign | (away ? kHalfInf : kHalfMax);
    if (exponent < kHalfSmallestExponent) return away ? (sign | kHalfNormal) : sign;
    uint mantissa = magnitude & kFloatMantissa;
    uint half = uint(exponent + kHalfBias) << 10 | (mantissa >> 13);
    if (away && (mantissa & kDroppedMantissa) != 0u) half += 1u;
    return sign | half;
}

void store_box16(uint offset, float3 lo, float3 hi) {
    g_dst.Store(offset, half_directed(lo.x, false) | half_directed(lo.y, false) << 16);
    g_dst.Store(offset + 4u, half_directed(lo.z, false) | half_directed(hi.x, true) << 16);
    g_dst.Store(offset + 8u, half_directed(hi.y, true) | half_directed(hi.z, true) << 16);
}

float3 vertex_at(uint index) {
    uint byte = index * arg0;
    float x = asfloat(g_src0.Load(byte));
    float y = asfloat(g_src0.Load(byte + 4u));
    float z = arg1 == 1u ? 0.0 : asfloat(g_src0.Load(byte + 8u));
    return float3(x, y, z);
}

uint index_of(uint index) {
    if (arg2 == 0u) return index;
    if (is_null(src1_va)) return kInvalid;
    uint skew = src1_va.x & 3u;
    uint carry;
    uint byte = uadd_carry(skew, arg2 == 2u ? 4u * index : 2u * index, carry);
    if (carry != 0u) return kInvalid;
    uint word = g_src1.Load(byte & ~3u);
    if (arg2 == 2u) return word;
    return (byte & 2u) != 0u ? word >> 16 : word & 0xffffu;
}

float3 transformed(float3 position) {
    if (is_null(src2_va)) return position;
    float3 result = 0.0;
    [unroll] for (uint row = 0u; row < 3u; ++row) {
        uint offset = 16u * row;
        float4 row_m = float4(asfloat(g_src2.Load(offset)), asfloat(g_src2.Load(offset + 4u)),
                              asfloat(g_src2.Load(offset + 8u)), asfloat(g_src2.Load(offset + 12u)));
        result[row] = dot(row_m.xyz, position) + row_m.w;
    }
    return result;
}

void write_inactive_leaf(uint element, uint primitive) {
    uint offset = elements_off + 64u * element;
    [unroll] for (uint slot = 0u; slot < 4u; ++slot) {
        g_dst.Store(offset + 12u * slot, kQuietNan);
        g_dst.Store(offset + 12u * slot + 4u, kQuietNan);
        g_dst.Store(offset + 12u * slot + 8u, kQuietNan);
    }
    g_dst.Store(offset + 48u, geometry);
    g_dst.Store(offset + 52u, primitive);
    g_dst.Store(offset + 56u, kInvalid);
    g_dst.Store(offset + 60u, kRemapIdentity);
    float3 lo, hi;
    empty_box(lo, hi);
    store_box(element, lo, hi);
}

void build_triangle(uint tri) {
    uint ids[3];
    bool inactive = false;
    [unroll] for (uint corner = 0u; corner < 3u; ++corner) {
        ids[corner] = index_of(3u * tri + corner);
        if (ids[corner] >= bias.y) inactive = true;
    }
    uint element = first + tri;
    if (inactive) {
        write_inactive_leaf(element, tri);
        return;
    }
    float3 verts[3];
    [unroll] for (uint corner = 0u; corner < 3u; ++corner) verts[corner] = transformed(vertex_at(ids[corner]));
    uint offset = elements_off + 64u * element;
    [unroll] for (uint corner = 0u; corner < 3u; ++corner) {
        g_dst.Store(offset + 12u * corner, asuint(verts[corner].x));
        g_dst.Store(offset + 12u * corner + 4u, asuint(verts[corner].y));
        g_dst.Store(offset + 12u * corner + 8u, asuint(verts[corner].z));
    }
    g_dst.Store(offset + 36u, asuint(verts[2].x));
    g_dst.Store(offset + 40u, asuint(verts[2].y));
    g_dst.Store(offset + 44u, asuint(verts[2].z));
    g_dst.Store(offset + 48u, geometry);
    g_dst.Store(offset + 52u, tri);
    g_dst.Store(offset + 56u, kInvalid);
    g_dst.Store(offset + 60u, kRemapIdentity);
    bool nan_any = any(isnan(verts[0])) || any(isnan(verts[1])) || any(isnan(verts[2]));
    float3 lo = min(min(verts[0], verts[1]), verts[2]);
    float3 hi = max(max(verts[0], verts[1]), verts[2]);
    if (nan_any) empty_box(lo, hi);
    store_box(element, lo, hi);
    grow_centroid_bounds(lo, hi);
}

bool blas_in_window(uint2 address) {
    if (is_null(address) || address.y != 0u) return false;
    return address.x <= bias.x;
}

float det3(float a00, float a01, float a02, float a10, float a11, float a12, float a20, float a21, float a22) {
    return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
}

float mat_at(float m[12], uint row, uint col) { return m[row * 4u + col]; }

float cofactor(float m[12], uint row, uint col) {
    uint r1 = (col + 1u) % 3u;
    uint r2 = (col + 2u) % 3u;
    uint c1 = (row + 1u) % 3u;
    uint c2 = (row + 2u) % 3u;
    return mat_at(m, r1, c1) * mat_at(m, r2, c2) - mat_at(m, r1, c2) * mat_at(m, r2, c1);
}

void build_instance(uint index) {
    uint desc = 64u * index;
    float m[12];
    [unroll] for (uint k = 0u; k < 12u; ++k) m[k] = asfloat(g_src0.Load(desc + 4u * k));
    uint id_mask = g_src0.Load(desc + 48u);
    uint contrib = g_src0.Load(desc + 52u);
    uint2 blas_va = uint2(g_src0.Load(desc + 56u), g_src0.Load(desc + 60u));
    uint offset = elements_off + 128u * index;
    [unroll] for (uint k = 0u; k < 12u; ++k) g_dst.Store(offset + 4u * k, asuint(m[k]));
    g_dst.Store(offset + 48u, id_mask);
    g_dst.Store(offset + 52u, contrib);
    float3 lo, hi;
    empty_box(lo, hi);
    uint root = kInvalid;
    if (blas_in_window(blas_va)) {
        root = g_window.Load(blas_va.x + 48u);
        float3 blo = float3(asfloat(g_window.Load(blas_va.x + 16u)), asfloat(g_window.Load(blas_va.x + 20u)),
                            asfloat(g_window.Load(blas_va.x + 24u)));
        float3 bhi = float3(asfloat(g_window.Load(blas_va.x + 28u)), asfloat(g_window.Load(blas_va.x + 32u)),
                            asfloat(g_window.Load(blas_va.x + 36u)));
        if (!box_empty(blo, bhi) && (id_mask >> 24) != 0u) {
            [unroll] for (uint corner = 0u; corner < 8u; ++corner) {
                float3 corner_p = float3((corner & 1u) != 0u ? bhi.x : blo.x, (corner & 2u) != 0u ? bhi.y : blo.y,
                                      (corner & 4u) != 0u ? bhi.z : blo.z);
                float3 world = float3(m[0] * corner_p.x + m[1] * corner_p.y + m[2] * corner_p.z + m[3],
                                      m[4] * corner_p.x + m[5] * corner_p.y + m[6] * corner_p.z + m[7],
                                      m[8] * corner_p.x + m[9] * corner_p.y + m[10] * corner_p.z + m[11]);
                lo = min(lo, world);
                hi = max(hi, world);
            }
        }
    }
    bool identity = m[0] == 1.0 && m[1] == 0.0 && m[2] == 0.0 && m[4] == 0.0 && m[5] == 1.0 && m[6] == 0.0 &&
                    m[8] == 0.0 && m[9] == 0.0 && m[10] == 1.0;
    g_dst.Store(offset + 56u, (root & ~kInstanceFlagsMask) | (identity ? kIdentityRotation : 0u));
    g_dst.Store(offset + 60u, (blas_va.x >> 8) | (blas_va.y << 24));
    float det = det3(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    float3 translation = float3(m[3], m[7], m[11]);
    [unroll] for (uint row = 0u; row < 3u; ++row) {
        float3 inv_row = 0.0;
        if (det != 0.0) {
            inv_row = float3(cofactor(m, row, 0u), cofactor(m, row, 1u), cofactor(m, row, 2u)) / det;
        }
        float it = -(inv_row.x * translation.x + inv_row.y * translation.y + inv_row.z * translation.z);
        g_dst.Store(offset + 64u + 16u * row, asuint(inv_row.x));
        g_dst.Store(offset + 68u + 16u * row, asuint(inv_row.y));
        g_dst.Store(offset + 72u + 16u * row, asuint(inv_row.z));
        g_dst.Store(offset + 76u + 16u * row, asuint(it));
    }
    g_dst.Store(offset + 112u, index);
    g_dst.Store(offset + 116u, kInvalid);
    g_dst.Store(offset + 120u, 0u);
    g_dst.Store(offset + 124u, 0u);
    if (det == 0.0) empty_box(lo, hi);
    store_box(index, lo, hi);
    grow_centroid_bounds(lo, hi);
}

uint spread10(uint value) {
    value &= 0x3ffu;
    value = (value | (value << 16)) & 0x030000ffu;
    value = (value | (value << 8)) & 0x0300f00fu;
    value = (value | (value << 4)) & 0x030c30c3u;
    value = (value | (value << 2)) & 0x09249249u;
    return value;
}

void morton(uint index) {
    uint key = kMortonEmpty;
    float3 lo, hi;
    load_box(index, lo, hi);
    if (!box_empty(lo, hi)) {
        float3 cmin = float3(unordered(g_scratch.Load(0u)), unordered(g_scratch.Load(4u)), unordered(g_scratch.Load(8u)));
        float3 cmax = float3(unordered(g_scratch.Load(12u)), unordered(g_scratch.Load(16u)), unordered(g_scratch.Load(20u)));
        float3 extent = max(cmax - cmin, float3(1e-30, 1e-30, 1e-30));
        float3 qf = clamp((((lo + hi) * 0.5) - cmin) / extent, 0.0, 1.0) * 1023.0;
        uint3 q = uint3(qf);
        key = spread10(q.x) << 2 | spread10(q.y) << 1 | spread10(q.z);
    }
    g_scratch.Store(keys_off() + 4u * index, key);
    g_scratch.Store(vals_off() + 4u * index, index);
}

void small_sort(uint thread) {
    uint n = sort_size;
    uint padded = 1u;
    while (padded < n) padded <<= 1u;
    for (uint i = thread; i < padded; i += kGroup) {
        small_keys[i] = i < n ? g_scratch.Load(keys_off() + 4u * i) : kInvalid;
        small_vals[i] = i < n ? g_scratch.Load(vals_off() + 4u * i) : kInvalid;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint span = 2u; span <= padded; span <<= 1u) {
        for (uint j = span >> 1u; j > 0u; j >>= 1u) {
            for (uint i = thread; i < padded; i += kGroup) {
                uint other = i ^ j;
                if (other > i) {
                    uint key_i = small_keys[i];
                    uint key_l = small_keys[other];
                    uint val_i = small_vals[i];
                    uint val_l = small_vals[other];
                    bool greater = key_i > key_l || (key_i == key_l && val_i > val_l);
                    if (greater == ((i & span) == 0u)) {
                        small_keys[i] = key_l;
                        small_keys[other] = key_i;
                        small_vals[i] = val_l;
                        small_vals[other] = val_i;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    for (uint i = thread; i < n; i += kGroup) {
        g_scratch.Store(keys_off() + 4u * i, small_keys[i]);
        g_scratch.Store(vals_off() + 4u * i, small_vals[i]);
    }
}

uint src_keys() { return (first & 1u) == 0u ? keys_off() : keys2_off(); }
uint src_vals() { return (first & 1u) == 0u ? vals_off() : vals2_off(); }
uint dst_keys() { return (first & 1u) == 0u ? keys2_off() : keys_off(); }
uint dst_vals() { return (first & 1u) == 0u ? vals2_off() : vals_off(); }
uint digit_of(uint key) { return (key >> (kRadixBits * first)) & (kRadixBins - 1u); }

void radix_count(uint group, uint thread) {
    if (thread < kRadixBins) radix_hist[thread] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint begin = group * kRadixTile;
    uint end = min(begin + kRadixTile, sort_size);
    for (uint i = begin + thread; i < end; i += kGroup) {
        uint ignored;
        InterlockedAdd(radix_hist[digit_of(g_scratch.Load(src_keys() + 4u * i))], 1u, ignored);
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread < kRadixBins) g_scratch.Store(counts_off() + 4u * (thread * radix_tiles() + group), radix_hist[thread]);
}

void scan(uint thread, uint src, uint dst, uint n, uint total) {
    uint per = (n + 63u) / 64u;
    uint begin = min(thread * per, n);
    uint end = min(begin + per, n);
    uint sum = 0u;
    for (uint i = begin; i < end; ++i) sum += g_scratch.Load(src + 4u * i);
    radix_part[thread] = sum;
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0u) {
        uint running = 0u;
        [unroll] for (uint k = 0u; k < 64u; ++k) {
            uint value = radix_part[k];
            radix_part[k] = running;
            running += value;
        }
        if (total != kInvalid) g_scratch.Store(total, running);
    }
    GroupMemoryBarrierWithGroupSync();
    uint running = radix_part[thread];
    for (uint i = begin; i < end; ++i) {
        uint value = g_scratch.Load(src + 4u * i);
        g_scratch.Store(dst + 4u * i, running);
        running += value;
    }
}

void radix_scatter(uint group, uint thread) {
    if (thread < kRadixBins) radix_base[thread] = g_scratch.Load(offsets_off() + 4u * (thread * radix_tiles() + group));
    uint begin = group * kRadixTile;
    uint end = min(begin + kRadixTile, sort_size);
    for (uint chunk = begin; chunk < end; chunk += kGroup) {
        uint i = chunk + thread;
        bool live = i < end;
        uint key = live ? g_scratch.Load(src_keys() + 4u * i) : 0u;
        uint value = live ? g_scratch.Load(src_vals() + 4u * i) : 0u;
        uint digit = digit_of(key);
        [unroll] for (uint bin = 0u; bin < kRadixBins; ++bin) radix_rank[bin][thread] = (live && bin == digit) ? 1u : 0u;
        GroupMemoryBarrierWithGroupSync();
        for (uint step = 1u; step < kGroup; step <<= 1u) {
            uint add[kRadixBins];
            [unroll] for (uint bin = 0u; bin < kRadixBins; ++bin) add[bin] = thread >= step ? radix_rank[bin][thread - step] : 0u;
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint bin = 0u; bin < kRadixBins; ++bin) radix_rank[bin][thread] += add[bin];
            GroupMemoryBarrierWithGroupSync();
        }
        if (live) {
            uint place = radix_base[digit] + radix_rank[digit][thread] - 1u;
            g_scratch.Store(dst_keys() + 4u * place, key);
            g_scratch.Store(dst_vals() + 4u * place, value);
        }
        GroupMemoryBarrierWithGroupSync();
        if (thread < kRadixBins) radix_base[thread] += radix_rank[thread][63];
        GroupMemoryBarrierWithGroupSync();
    }
}

uint leaf_off(uint element) { return elements_off + 64u * element; }

uint3 leaf_vertex(uint element, uint slot) {
    uint offset = leaf_off(element) + 12u * slot;
    return uint3(g_dst.Load(offset), g_dst.Load(offset + 4u), g_dst.Load(offset + 8u));
}

void store_vertex(uint element, uint slot, uint3 value) {
    uint offset = leaf_off(element) + 12u * slot;
    g_dst.Store(offset, value.x);
    g_dst.Store(offset + 4u, value.y);
    g_dst.Store(offset + 8u, value.z);
}

struct Quad {
    uint e[4];
    uint kids[4];
};

void pair_children(inout Quad quad) {
    uint3 verts[4][3];
    uint geo[4];
    bool live[4];
    bool used[4];
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        live[c] = false;
        used[c] = false;
        geo[c] = 0u;
        [unroll] for (uint k = 0u; k < 3u; ++k) verts[c][k] = uint3(0u, 0u, 0u);
        if (quad.e[c] == kInvalid) continue;
        geo[c] = g_dst.Load(leaf_off(quad.e[c]) + 48u);
        [unroll] for (uint k = 0u; k < 3u; ++k) verts[c][k] = leaf_vertex(quad.e[c], k);
        float3 lo, hi;
        load_box(quad.e[c], lo, hi);
        live[c] = !box_empty(lo, hi);
    }
    [loop] for (uint c = 0u; c < 4u; ++c) {
        if (used[c] || !live[c]) continue;
        used[c] = true;
        [loop] for (uint d = c + 1u; d < 4u; ++d) {
            if (used[d] || !live[d] || geo[d] != geo[c]) continue;
            uint edge = 3u;
            uint sibling = 3u;
            [loop] for (uint a = 0u; a < 3u && edge == 3u; ++a) {
                uint3 p = verts[c][a];
                uint3 q = verts[c][(a + 1u) % 3u];
                if (all(p == q)) continue;
                [unroll] for (uint b = 0u; b < 3u; ++b) {
                    if (all(verts[d][b] == q) && all(verts[d][(b + 1u) % 3u] == p)) {
                        edge = a;
                        sibling = b;
                        break;
                    }
                }
            }
            if (edge == 3u) continue;
            used[d] = true;
            store_vertex(quad.e[c], 0u, verts[c][(edge + 2u) % 3u]);
            store_vertex(quad.e[c], 1u, verts[c][edge]);
            store_vertex(quad.e[c], 2u, verts[c][(edge + 1u) % 3u]);
            store_vertex(quad.e[c], 3u, verts[d][(sibling + 2u) % 3u]);
            g_dst.Store(leaf_off(quad.e[c]) + 56u, g_dst.Load(leaf_off(quad.e[d]) + 52u));
            uint remap0 = (5u - edge) % 3u | ((6u - edge) % 3u) << 2;
            uint remap1 = (6u - sibling) % 3u | ((7u - sibling) % 3u) << 2;
            g_dst.Store(leaf_off(quad.e[c]) + 60u, remap0 | remap1 << 8);
            quad.kids[d] = (quad.kids[c] & ~7u) | kTri1;
            break;
        }
    }
}

uint sorted_key(int index) { return g_scratch.Load(keys_off() + 4u * uint(index)); }

int delta(int i, int j) {
    if (j < 0 || j >= int(elements)) return -1;
    uint a = sorted_key(i);
    uint b = sorted_key(j);
    if (a == b) return 64 - firstbithigh(uint(i ^ j));
    return 31 - firstbithigh(a ^ b);
}

void karras(int i) {
    int direction = delta(i, i + 1) - delta(i, i - 1) > 0 ? 1 : -1;
    int dmin = delta(i, i - direction);
    int lmax = 2;
    while (delta(i, i + lmax * direction) > dmin) lmax <<= 1;
    int length = 0;
    for (int step = lmax >> 1; step >= 1; step >>= 1) {
        if (delta(i, i + (length + step) * direction) > dmin) length += step;
    }
    int j = i + length * direction;
    int dnode = delta(i, j);
    int split_steps = 0;
    int remain = length;
    do {
        remain = (remain + 1) >> 1;
        if (delta(i, i + (split_steps + remain) * direction) > dnode) split_steps += remain;
    } while (remain > 1);
    int split = i + split_steps * direction + min(direction, 0);
    uint left = min(i, j) == split ? (kLeafBit | uint(split)) : uint(split);
    uint right = max(i, j) == split + 1 ? (kLeafBit | uint(split + 1)) : uint(split + 1);
    g_scratch.Store(left_off() + 4u * uint(i), left);
    g_scratch.Store(right_off() + 4u * uint(i), right);
    if ((left & kLeafBit) == 0u) g_scratch.Store(bparent_off() + 4u * left, uint(i));
    if ((right & kLeafBit) == 0u) g_scratch.Store(bparent_off() + 4u * right, uint(i));
    if (i == 0) g_scratch.Store(bparent_off(), kInvalid);
}

void kept_flags(uint group, uint thread) {
    if (thread == 0u) kept_count = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint begin = group * kRadixTile;
    uint end = min(begin + kRadixTile, inner_count());
    for (uint i = begin + thread; i < end; i += kGroup) {
        uint depth = 0u;
        uint parent = g_scratch.Load(bparent_off() + 4u * i);
        for (uint guard = 0u; parent != kInvalid && guard < kDepthGuard; ++guard) {
            ++depth;
            parent = g_scratch.Load(bparent_off() + 4u * parent);
        }
        uint kept = (depth & 1u) == 0u ? 1u : 0u;
        g_scratch.Store(kept_off() + 4u * i, kept);
        if (kept != 0u) {
            uint ignored;
            InterlockedAdd(kept_count, 1u, ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0u) g_scratch.Store(ktile_counts_off() + 4u * group, kept_count);
}

void kept_index(uint group, uint thread) {
    uint base = g_scratch.Load(ktile_offsets_off() + 4u * group);
    uint begin = group * kRadixTile;
    uint end = min(begin + kRadixTile, inner_count());
    for (uint chunk = begin; chunk < end; chunk += kGroup) {
        uint i = chunk + thread;
        uint flag = i < end ? g_scratch.Load(kept_off() + 4u * i) : 0u;
        kept_scan[thread] = flag;
        GroupMemoryBarrierWithGroupSync();
        for (uint step = 1u; step < kGroup; step <<= 1u) {
            uint add = thread >= step ? kept_scan[thread - step] : 0u;
            GroupMemoryBarrierWithGroupSync();
            kept_scan[thread] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (flag != 0u) g_scratch.Store(kmap_off() + 4u * i, base + kept_scan[thread] - 1u);
        base += kept_scan[63];
        GroupMemoryBarrierWithGroupSync();
    }
}

void clear_accum(uint node) {
    uint offset = accum_off() + 32u * node;
    g_scratch.Store(offset, 0u);
    g_scratch.Store(offset + 4u, kInvalid);
    g_scratch.Store(offset + 8u, kInvalid);
    g_scratch.Store(offset + 12u, kInvalid);
    g_scratch.Store(offset + 16u, 0u);
    g_scratch.Store(offset + 20u, 0u);
    g_scratch.Store(offset + 24u, 0u);
}

uint child_element(uint child) {
    return (child & kLeafBit) != 0u ? g_scratch.Load(vals_off() + 4u * (child & ~kLeafBit)) : kInvalid;
}

uint child_ptr(uint child) {
    if ((child & kLeafBit) != 0u) return element_ptr(child_element(child));
    return node_ptr(g_scratch.Load(kmap_off() + 4u * child));
}

void store_slot_box(uint offset, uint slot, bool box16, float3 lo, float3 hi) {
    if (box16) {
        store_box16(offset + 16u + 12u * slot, lo, hi);
        return;
    }
    uint base = offset + 16u + 24u * slot;
    g_dst.Store(base, asuint(lo.x));
    g_dst.Store(base + 4u, asuint(lo.y));
    g_dst.Store(base + 8u, asuint(lo.z));
    g_dst.Store(base + 12u, asuint(hi.x));
    g_dst.Store(base + 16u, asuint(hi.y));
    g_dst.Store(base + 20u, asuint(hi.z));
}

void build_node(uint index) {
    Quad quad;
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        quad.e[c] = kInvalid;
        quad.kids[c] = kInvalid;
    }
    uint node = 0u;
    if (elements <= 1u) {
        if (elements == 1u) {
            quad.e[0] = g_scratch.Load(vals_off());
            quad.kids[0] = element_ptr(quad.e[0]);
        }
    } else {
        if (g_scratch.Load(kept_off() + 4u * index) == 0u) return;
        node = g_scratch.Load(kmap_off() + 4u * index);
        if (node >= arg0) return;
        uint n = 0u;
        uint halves[2] = {g_scratch.Load(left_off() + 4u * index), g_scratch.Load(right_off() + 4u * index)};
        [unroll] for (uint half = 0u; half < 2u; ++half) {
            uint child = halves[half];
            uint grand[2] = {child, child};
            uint count_g = 1u;
            if ((child & kLeafBit) == 0u) {
                grand[0] = g_scratch.Load(left_off() + 4u * child);
                grand[1] = g_scratch.Load(right_off() + 4u * child);
                count_g = 2u;
            }
            for (uint g = 0u; g < count_g; ++g) {
                quad.e[n] = child_element(grand[g]);
                quad.kids[n] = child_ptr(grand[g]);
                ++n;
            }
        }
        if (!is_tlas() && (flags & kFlagPair) != 0u) pair_children(quad);
    }
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        if (quad.e[c] != kInvalid) g_scratch.Store(tptr_off() + 4u * quad.e[c], quad.kids[c]);
    }
    uint me = node_ptr(node);
    uint offset = nodes_off + node_size() * node;
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        g_dst.Store(offset + 4u * c, quad.kids[c]);
        if (quad.kids[c] != kInvalid) {
            g_dst.Store(parents_off + 4u * (ptr_unit(quad.kids[c]) - elements_off / 64u), me);
            continue;
        }
        float3 lo, hi;
        empty_box(lo, hi);
        store_slot_box(offset, c, is_box16(), lo, hi);
    }
    if (!is_box16()) {
        [unroll] for (uint k = 0u; k < 4u; ++k) g_dst.Store(offset + 112u + 4u * k, 0u);
    }
    clear_accum(node);
}

uint parent_of(uint pointer) {
    return g_dst.Load(parents_off + 4u * (ptr_unit(pointer) - elements_off / 64u));
}

uint node_index(uint pointer) {
    return (ptr_unit(pointer) * 64u - nodes_off) / (ptr_box16(pointer) ? 64u : 128u);
}

void accum_box(uint node, out float3 lo, out float3 hi) {
    uint offset = accum_off() + 32u * node;
    uint words[6];
    [unroll] for (uint k = 0u; k < 6u; ++k) g_scratch.InterlockedOr(offset + 4u * (1u + k), 0u, words[k]);
    if (words[0] == kInvalid) {
        empty_box(lo, hi);
        return;
    }
    lo = float3(unordered(words[0]), unordered(words[1]), unordered(words[2]));
    hi = float3(unordered(words[3]), unordered(words[4]), unordered(words[5]));
}

void propagate(uint element) {
    uint me = arg2 == 1u ? element_ptr(element) : g_scratch.Load(tptr_off() + 4u * element);
    float3 lo, hi;
    load_box(element, lo, hi);
    uint parent = parent_of(me);
    if (is_tlas()) g_dst.Store(elements_off + 128u * element + 116u, parent);
    for (uint guard = 0u; parent != kInvalid && guard < kDepthGuard; ++guard) {
        uint offset = ptr_unit(parent) * 64u;
        uint children = 0u;
        [unroll] for (uint c = 0u; c < 4u; ++c) {
            uint kid = g_dst.Load(offset + 4u * c);
            if (kid == kInvalid) continue;
            ++children;
            if (kid == me) store_slot_box(offset, c, ptr_box16(parent), lo, hi);
        }
        uint node = node_index(parent);
        uint accum = accum_off() + 32u * node;
        if (!box_empty(lo, hi)) {
            uint ignored;
            g_scratch.InterlockedMin(accum + 4u, ordered(lo.x), ignored);
            g_scratch.InterlockedMin(accum + 8u, ordered(lo.y), ignored);
            g_scratch.InterlockedMin(accum + 12u, ordered(lo.z), ignored);
            g_scratch.InterlockedMax(accum + 16u, ordered(hi.x), ignored);
            g_scratch.InterlockedMax(accum + 20u, ordered(hi.y), ignored);
            g_scratch.InterlockedMax(accum + 24u, ordered(hi.z), ignored);
        }
        DeviceMemoryBarrier();
        uint arrived;
        g_scratch.InterlockedAdd(accum, 1u, arrived);
        DeviceMemoryBarrier();
        if (arrived + 1u != children) return;
        accum_box(node, lo, hi);
        me = parent;
        parent = parent_of(me);
    }
}

void build_header() {
    float3 lo, hi;
    accum_box(0u, lo, hi);
    g_dst.Store(16u, asuint(lo.x));
    g_dst.Store(20u, asuint(lo.y));
    g_dst.Store(24u, asuint(lo.z));
    g_dst.Store(28u, asuint(hi.x));
    g_dst.Store(32u, asuint(hi.y));
    g_dst.Store(36u, asuint(hi.z));
    if (arg2 == 1u) return;
    uint nodes = elements <= 1u ? 1u : g_scratch.Load(kScratchNodeCount);
    uint end = nodes_off + node_size() * nodes;
    if (is_box16()) {
        [unroll] for (uint k = 0u; k < kHeaderDwords; ++k) g_dst.Store(end + 4u * k, 0u);
        end += 64u;
    }
    g_dst.Store(0u, (is_tlas() ? 0u : kFlagBottomLevel) | kVersion2403);
    g_dst.Store(4u, end / 64u - 1u);
    g_dst.Store(8u, 0u);
    g_dst.Store(12u, elements);
    g_dst.Store(40u, nodes_off);
    g_dst.Store(44u, 0u);
    g_dst.Store(48u, node_ptr(0u));
    g_dst.Store(52u, elements_off);
    g_dst.Store(56u, parents_off);
    g_dst.Store(60u, 0u);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_thread : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
    uint i = dispatch_id.x;
    uint thread = group_thread.x;
    uint group = group_id.x;
    if (mode == kModeRadixCount) {
        radix_count(group, thread);
        return;
    }
    if (mode == kModeRadixScan) {
        scan(thread, counts_off(), offsets_off(), kRadixBins * radix_tiles(), kInvalid);
        return;
    }
    if (mode == kModeRadixScatter) {
        radix_scatter(group, thread);
        return;
    }
    if (mode == kModeSmallSort) {
        small_sort(thread);
        return;
    }
    if (mode == kModeKept) {
        kept_flags(group, thread);
        return;
    }
    if (mode == kModeKeptScan) {
        scan(thread, ktile_counts_off(), ktile_offsets_off(), kept_tile_count(), kScratchNodeCount);
        return;
    }
    if (mode == kModeKeptIndex) {
        kept_index(group, thread);
        return;
    }
    if (mode == kModeHeader) {
        if (i == 0u) build_header();
        return;
    }
    if (i >= count) return;
    if (mode == kModePostbuild) {
        if (i == 0u) {
            g_dst.Store(0u, (g_src0.Load(4u) + 1u) * 64u);
            g_dst.Store(4u, 0u);
        }
        return;
    }
    if (mode == kModeCopy) {
        if (4u * i < (g_src0.Load(4u) + 1u) * 64u) g_dst.Store(4u * i, g_src0.Load(4u * i));
        return;
    }
    if (mode == kModeInit) {
        g_dst.Store(parents_off + 4u * i, kInvalid);
        if (i < 3u) {
            g_scratch.Store(4u * i, kInvalid);
            g_scratch.Store(12u + 4u * i, 0u);
        }
    } else if (mode == kModeTriangles) {
        build_triangle(i);
    } else if (mode == kModeInstances) {
        build_instance(i);
    } else if (mode == kModeMorton) {
        morton(i);
    } else if (mode == kModeKarras) {
        karras(int(i));
    } else if (mode == kModeNodes) {
        build_node(i);
    } else if (mode == kModePropagate) {
        propagate(i);
    } else if (mode == kModeClear) {
        clear_accum(i);
    }
}
