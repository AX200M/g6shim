// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

Texture2D<float4> plane0 : register(t0);
Texture2D<float4> plane1 : register(t1);
Texture2D<float4> plane2 : register(t2);
Texture2D<float4> plane3 : register(t3);
SamplerState linear_clamp : register(s0);

cbuffer PlaneCount : register(b0) {
    uint plane_count;
};

struct Interpolant {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Interpolant present_vs(uint id : SV_VertexID) {
    Interpolant output;
    float2 corner = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    output.uv = corner;
    return output;
}

float3 composite(float3 under, float4 over) {
    return over.rgb + under * (1.0 - over.a);
}

float4 present_ps(Interpolant input) : SV_Target {
    float3 rgb = plane0.Sample(linear_clamp, input.uv).rgb;
    if (plane_count > 1) {
        rgb = composite(rgb, plane1.Sample(linear_clamp, input.uv));
    }
    if (plane_count > 2) {
        rgb = composite(rgb, plane2.Sample(linear_clamp, input.uv));
    }
    if (plane_count > 3) {
        rgb = composite(rgb, plane3.Sample(linear_clamp, input.uv));
    }
    return float4(rgb, 1.0);
}
