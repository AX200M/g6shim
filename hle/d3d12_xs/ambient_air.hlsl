// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ambient_common.hlsli"

RWTexture3D<float3> scattered : register(u0);

[RootSignature(AMBIENT_ROOT)]
[numthreads(8, 8, 2)]
void clear_air(uint3 at : SV_DispatchThreadID) {
    uint w, h, d;
    scattered.GetDimensions(w, h, d);
    if (at.x >= w || at.y >= h || at.z >= d) return;
    uint2 q = min(uint2((float2(at.xy) + 0.5) / float2(w, h) * float2(POINTS_X, POINTS_Y)),
                  uint2(POINTS_X - 1, POINTS_Y - 1));
    uint first = (q.y * POINTS_X + q.x) * 3;
    float3 seen_there = float3(points[first].w, points[first + 1].w, points[first + 2].w);
    float far_share = float(at.z) / float(d);
    float cap = air_cap * lerp(dot(cells[CELLS].rgb, LUMA), dot(seen_there, LUMA), far_share);
    float3 c = scattered[at];
    float has = dot(c, LUMA);
    if (has > cap) scattered[at] = c * (cap / has);
}
