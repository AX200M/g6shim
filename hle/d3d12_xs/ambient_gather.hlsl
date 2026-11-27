// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ambient_common.hlsli"

[RootSignature(AMBIENT_ROOT)]
[numthreads(8, 4, 1)]
void place(uint3 at : SV_DispatchThreadID) {
    float2 size = float2(width, height);
    float3 p = seen(int2((float2(at.xy) + 0.5) * size / float2(CELLS_X, CELLS_Y)), size);
    cells[CELLS + 1 + at.y * CELLS_X + at.x] = float4(p, p.z * p.z);
}

[RootSignature(AMBIENT_ROOT)]
[numthreads(8, 2, 1)]
void gather(uint3 at : SV_DispatchThreadID) {
    float2 size = float2(width, height);
    float3 p = seen(int2((float2(at.xy) + 0.5) * size / float2(POINTS_X, POINTS_Y)), size);
    float r2 = 0.25 + 0.0025 * p.z * p.z;
    float whole = 0;
    float3 toward = 0;
    float3 light = 0;
    float3 red = 0;
    float3 green = 0;
    float3 blue = 0;
    for (uint i = 0; i < CELLS; i++) {
        float4 c = cells[CELLS + 1 + i];
        float3 l = cells[i].rgb;
        float3 d = c.xyz - p;
        float dd = dot(d, d);
        float w = c.w / (dd + r2);
        float3 dir = d / sqrt(dd + 0.25 * r2);
        whole += w;
        toward += w * dir;
        light += w * l;
        red += w * l.r * dir;
        green += w * l.g * dir;
        blue += w * l.b * dir;
    }
    whole = max(whole, 1e-6);
    light /= whole;
    toward /= whole;
    uint first = (at.y * POINTS_X + at.x) * 3;
    points[first] = float4(red / whole - light.r * toward, light.r);
    points[first + 1] = float4(green / whole - light.g * toward, light.g);
    points[first + 2] = float4(blue / whole - light.b * toward, light.b);
}
