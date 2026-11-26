// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

cbuffer Span : register(b0) {
    uint first;
    uint bytes;
    uint mode;
    uint piece_bytes;
};

RWByteAddressBuffer source : register(u0);
RWByteAddressBuffer hashes : register(u1);
RWByteAddressBuffer flags : register(u2);

groupshared uint4 partials[64];

uint4 mix_block(uint4 hash, uint4 value) {
    uint4 mixed = (hash ^ value) * uint4(2246822519u, 3266489917u, 668265263u, 374761393u);
    return uint4(mixed.y, mixed.z, mixed.w, mixed.x ^ (mixed.x >> 16));
}

[numthreads(64, 1, 1)]
void main(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex) {
    uint piece = first + group_id.x;
    uint start = piece * piece_bytes;
    uint end = start + piece_bytes;
    if (end > bytes) end = bytes;
    uint4 hash = uint4(0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au) + lane;
    uint step = 64u * 16u;
    for (uint at = start + lane * 16u; at + 16u <= end; at += step)
        hash = mix_block(hash, source.Load4(at));
    partials[lane] = hash;
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0u) return;
    hash = partials[0];
    [unroll]
    for (uint other = 1u; other < 64u; ++other)
        hash = mix_block(hash, partials[other]);
    if (mode == 0u) {
        hashes.Store4(piece * 16u, hash);
        return;
    }
    uint4 stored = hashes.Load4(piece * 16u);
    flags.Store(piece * 4u, any(hash != stored) ? 1u : 0u);
}
