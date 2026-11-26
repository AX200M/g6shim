// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later

RWTexture2D<unorm float> image : register(u0);

[numthreads(4, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float value = id.x == 1 ? 16384.0 / 65535.0 : 8192.0 / 65535.0;
    image[uint2(id.x, 0)] = value;
}
