// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

struct Interpolant {
    float4 position : SV_Position;
};

Interpolant equal_vs(uint id : SV_VertexID) {
    Interpolant output;
    float2 corner = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 16384.0 / 65535.0, 1.0);
    return output;
}
float4 equal_ps(Interpolant input) : SV_Target {
    float keep = input.position.w > 0.0 ? 1.0 : 1.0;
    return float4(keep, keep, keep, keep);
}
