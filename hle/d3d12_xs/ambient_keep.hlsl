// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ambient_common.hlsli"

RWTexture2D<float3> radiance : register(u0);

[RootSignature(AMBIENT_ROOT)]
[numthreads(8, 4, 1)]
void keep(uint3 at : SV_DispatchThreadID) {
    uint w, h;
    radiance.GetDimensions(w, h);
    float3 sum = 0;
    for (uint y = 0; y < 5; y++) {
        for (uint x = 0; x < 5; x++) {
            uint2 p = uint2((float2(at.xy) + (float2(x, y) + 0.5) / 5.0) * float2(w, h) / float2(CELLS_X, CELLS_Y));
            float3 c = max(radiance[p], 0.0);
            c = any(isnan(c)) || any(isinf(c)) ? 0.0 : c;
            sum += c / max(1.0, dot(c, LUMA) / 20.0);
        }
    }
    cells[at.y * CELLS_X + at.x] = float4(sum / 25.0, 1.0);
}

[RootSignature(AMBIENT_ROOT)]
[numthreads(1, 1, 1)]
void mean() {
    float3 sum = 0;
    for (uint i = 0; i < CELLS; i++) sum += cells[i].rgb;
    float4 was = cells[CELLS];
    float3 now = sum / CELLS;
    float a = max(dot(now, LUMA), 1e-5);
    float b = max(dot(was.rgb, LUMA), 1e-5);
    bool jumped = jump > 0.0 && max(a / b, b / a) > jump;
    float age = was.a > 0.0 && !jumped ? was.a + seconds : 0.001;
    cells[CELLS] = float4(was.a > 0.0 ? lerp(was.rgb, now, jumped ? 1.0 : follow) : now, age);
}
