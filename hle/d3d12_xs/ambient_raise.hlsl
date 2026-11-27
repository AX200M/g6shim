// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ambient_common.hlsli"

RWTexture2D<uint> light : register(u0);

float3 unpack(uint v) {
    return float3(v & 511, (v >> 9) & 511, (v >> 18) & 511) * exp2(float(v >> 27) - 24.0);
}

uint pack(float3 c) {
    c = clamp(c, 0.0, 65408.0);
    float top = max(c.r, max(c.g, c.b));
    int e = clamp(int(floor(log2(max(top, 1e-9)))) + 16, 0, 31);
    float scale = exp2(float(24 - e));
    if (top * scale >= 511.5) {
        e = min(e + 1, 31);
        scale = exp2(float(24 - e));
    }
    uint3 m = min(uint3(c * scale + 0.5), 511u);
    return m.r | (m.g << 9) | (m.b << 18) | (uint(e) << 27);
}

[RootSignature(AMBIENT_ROOT)]
[numthreads(8, 8, 1)]
void raise(uint3 at : SV_DispatchThreadID) {
    float2 size = float2(width, height);
    if (at.x >= width || at.y >= height) return;
    int2 p = int2(at.xy);
    float3 c = seen(p, size);
    float3 l = seen(p + int2(-1, 0), size);
    float3 r = seen(p + int2(1, 0), size);
    float3 u = seen(p + int2(0, -1), size);
    float3 d = seen(p + int2(0, 1), size);
    float3 dx = abs(l.z - c.z) < abs(r.z - c.z) ? c - l : r - c;
    float3 dy = abs(u.z - c.z) < abs(d.z - c.z) ? c - u : d - c;
    float3 n = normalize(cross(dy, dx) + float3(0, 0, -1e-9));
    if (dot(n, c) > 0.0) n = -n;
    float open = 1.0;
    float px = min(radius * size.y / (2.0 * tan_half_y * c.z), 128.0);
    float noise = frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715))));
    if (strength > 0.0 && px >= 1.5 && c.z < 400.0) {
        float sum = 0.0;
        const int taps = 12;
        for (int i = 0; i < taps; i++) {
            float a = i * 2.399963 + noise * 6.2831853;
            float2 o = float2(cos(a), sin(a)) * (sqrt((i + 0.5) / taps) * px);
            float3 v = seen(p + int2(round(o)), size) - c;
            float vv = dot(v, v);
            float over = dot(v, n) - 0.004 * c.z - 0.005;
            sum += max(over, 0.0) / sqrt(vv + 1e-6) * saturate(1.0 - vv / (radius * radius));
        }
        open = saturate(1.0 - strength * 2.0 * sum / taps);
    }
    float3 found;
    if (constant_level > 0.0) {
        found = float3(1.0, 0.98, 0.94) * constant_level * open;
        light[p] = pack(found);
        return;
    }
    float2 g = (float2(p) + 0.5) / size * float2(POINTS_X, POINTS_Y) - 0.5 + (noise - 0.5) * 0.5;
    int2 g0 = int2(floor(g));
    float2 f = g - float2(g0);
    float4 sr = 0;
    float4 sg = 0;
    float4 sb = 0;
    float total = 0;
    for (int k = 0; k < 4; k++) {
        int2 q = clamp(g0 + int2(k & 1, k >> 1), int2(0, 0), int2(POINTS_X, POINTS_Y) - 1);
        float z = seen(int2((float2(q) + 0.5) * size / float2(POINTS_X, POINTS_Y)), size).z;
        float2 b = float2((k & 1) != 0 ? f.x : 1 - f.x, (k >> 1) != 0 ? f.y : 1 - f.y);
        float w = (b.x * b.y + 1e-3) / (0.02 + abs(z - c.z) / c.z);
        uint first = (q.y * POINTS_X + q.x) * 3;
        sr += w * points[first];
        sg += w * points[first + 1];
        sb += w * points[first + 2];
        total += w;
    }
    float3 all = float3(sr.w, sg.w, sb.w) / total;
    found = all + 1.2 * float3(dot(sr.xyz, n), dot(sg.xyz, n), dot(sb.xyz, n)) / total;
    found = clamp(found, 0.2 * all, 2.5 * all);
    found = lerp(found, cells[CELLS].rgb, 0.2) * level * open;
    float3 was = unpack(light[p]);
    if (was.g < 0.5 * min(was.r, was.b) && max(was.r, was.b) > 0.02) was = 0.0;
    float left = dot(was, LUMA);
    float bound = fresh > 0.0 ? lerp(1.0, most, saturate(cells[CELLS].a / fresh)) : most;
    float top = max(dot(found, LUMA) * bound, 1e-6);
    was *= min(1.0, top / max(left, 1e-6));
    float3 written = max(was, found);
    light[p] = pack(written);
    if (probe != 0 && (at.x & 15) == 0 && (at.y & 15) == 0) {
        uint ignored;
        stats.InterlockedAdd(0, 1, ignored);
        stats.InterlockedAdd(4, uint(min(left, 1000.0) * 256.0), ignored);
        stats.InterlockedAdd(8, uint(min(dot(found, LUMA), 1000.0) * 256.0), ignored);
        stats.InterlockedAdd(12, uint(min(dot(written, LUMA), 1000.0) * 256.0), ignored);
    }
}
