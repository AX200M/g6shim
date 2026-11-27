// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#define AMBIENT_ROOT "RootConstants(num32BitConstants=15, b0), DescriptorTable(SRV(t0)), DescriptorTable(UAV(u0)), UAV(u1), UAV(u2), UAV(u3)"
#define CELLS_X 64
#define CELLS_Y 36
#define CELLS (CELLS_X * CELLS_Y)
#define POINTS_X 160
#define POINTS_Y 90

cbuffer Settings : register(b0) {
    float level;
    float constant_level;
    float strength;
    float radius;
    float near_plane;
    float tan_half_y;
    uint width;
    uint height;
    float follow;
    float most;
    float air_cap;
    uint probe;
    float jump;
    float fresh;
    float seconds;
};

Texture2D<float> depth : register(t0);
RWStructuredBuffer<float4> cells : register(u1);
RWStructuredBuffer<float4> points : register(u2);
RWByteAddressBuffer stats : register(u3);

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float3 seen(int2 p, float2 size) {
    float d = depth.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0));
    float z = min(near_plane / max(d, 1e-7), 1000.0);
    float2 ndc = (float2(p) + 0.5) / size * 2.0 - 1.0;
    return float3(ndc.x * tan_half_y * size.x / size.y * z, -ndc.y * tan_half_y * z, z);
}
