# shader

The Series GPU's shader programs: GFX10 machine code, the intermediate forms raised from it, and its containers.

## Scalar register codes

| Code | Register |
|---|---|
| 0-105 | user and system SGPRs |
| 106-107 | VCC |
| 124 | M0 |
| 125 | null, no register |
| 126-127 | EXEC |

A wave64 vector instruction reads and writes both EXEC dwords.

## Mask tests

| Branch | Test |
|---|---|
| `s_cbranch_execz`, `s_cbranch_execnz` | EXEC |
| `s_cbranch_vccz`, `s_cbranch_vccnz` | VCC, over the lanes in EXEC |
| `s_cbranch_scc0`, `s_cbranch_scc1` | SCC |

## Pixel input VGPRs

One bit of `SPI_PS_INPUT_ADDR` per entry, in this order; each enabled entry takes the next VGPRs.

| Bit | VGPRs | Input |
|---|---|---|
| 0 | 2 | perspective sample barycentrics i, j |
| 1 | 2 | perspective center |
| 2 | 2 | perspective centroid |
| 3 | 3 | perspective pull-model barycentric |
| 4 | 2 | linear sample |
| 5 | 2 | linear center |
| 6 | 2 | linear centroid |
| 7 | 1 | line stipple |
| 8-11 | 1 each | position x, y, z, w |
| 12 | 1 | front face |
| 13 | 1 | ancillary |
| 14 | 1 | sample coverage |
| 15 | 1 | fixed-point position |

## Export targets

| Target | Meaning |
|---|---|
| 0-7 | colour render target |
| 8 | depth, stencil and sample mask |
| 9 | discarded |
| 12-16 | position x, y, z, w |
| 20 | primitive, from a merged or mesh stage |
| 32-63 | vertex parameter `n - 32`; bit 7 of the parameter marks a mesh per-primitive output |

## Inline constants

| Code | Value |
|---|---|
| 128-192 | 0 to 64 |
| 193-208 | -1 to -16 |
| 240-247 | ±0.5, ±1, ±2, ±4 |
| 248 | 0x3E22F983 |

## Root parameters

User data dword `i` arrives in SGPR `i` in the vertex, pixel and compute stages, and in SGPR `i + 8` in the merged stages.
A table or heap parameter holds the 32-bit heap address in its first dword, with the high half set to 0; a root descriptor holds a 64-bit buffer address in its first two.
