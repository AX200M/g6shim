// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

Texture2D<float> depth_plane : register(t0);
Texture2D<uint> stencil_plane : register(t1);

cbuffer StencilBit : register(b0) {
    uint stencil_bit;
};

struct Interpolant {
    float4 position : SV_Position;
};

Interpolant copy_vs(uint id : SV_VertexID) {
    Interpolant output;
    float2 corner = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    return output;
}

float copy_depth_ps(Interpolant input) : SV_Depth {
    return depth_plane.Load(int3(uint2(input.position.xy), 0));
}

uint copy_stencil_ps(Interpolant input) : SV_StencilRef {
    return stencil_plane.Load(int3(uint2(input.position.xy), 0));
}

void copy_stencil_bit_ps(Interpolant input) {
    uint value = stencil_plane.Load(int3(uint2(input.position.xy), 0));
    if (((value >> stencil_bit) & 1) == 0) discard;
}
