# xg_xs.dll

The console's texture-layout library. Titles ask it for the swizzle mode of a texture, create a texture computer, then read the layout of each plane and mip, address single elements, copy subresources between tiled and linear memory, and query 64 KiB tilings.

## Exports

| Export | Signature |
|---|---|
| `XGCreateTextureComputer` | `HRESULT (const ResourceDesc* desc, Computer** out)` |
| `XGSuggestSwizzleMode` | `HRESULT (const ResourceDesc* desc, UINT32 flags, UINT32* out)` |

`XGSuggestSwizzleMode` ignores `flags` and writes `0x100 | mode`.

## Suggested modes

| Resource | Mode |
|---|---|
| 1D texture | 0 (linear) |
| Depth-stencil flag | 24 (64 KiB `Z_X`) |
| Render-target flag | 27 (64 KiB `R_X`) |
| Otherwise | 5 (4 KiB `S`) |

## Resource description (0x38 bytes)

| Offset | Field |
|---|---|
| +0x00 | `UINT32` dimension (2 = 1D, 3 = 2D, 4 = 3D) |
| +0x08 | `UINT64` alignment |
| +0x10 | `UINT64` width in texels |
| +0x18 | `UINT32` height |
| +0x1C | `UINT16` depth or array size |
| +0x1E | `UINT16` mip levels (0 = full chain) |
| +0x20 | `UINT32` DXGI format |
| +0x24 | `UINT32` sample count |
| +0x28 | `UINT32` sample quality |
| +0x2C | `UINT32` layout (`0` = suggested, `1` = linear, `0x100 | mode`) |
| +0x30 | `UINT32` flags (1 = render target, 2 = depth-stencil) |

## Texture computer

The object starts with a function table of 13 pointers. Every method takes the computer as its first argument.

| Slot | Method |
|---|---|
| 0x00 | `AddRef` |
| 0x08 | `Release` |
| 0x10 | `Layout(LayoutOut*)`, returns `HRESULT` |
| 0x18 | size, not implemented |
| 0x20 | alignment, not implemented |
| 0x28 | mip offset, not implemented |
| 0x30 | `ElementOffset(plane, mip, x, y, z, sample, bit*)`, returns `UINT64` |
| 0x38 | element coordinate, not implemented |
| 0x40 | `CopyInto(tiled, plane, subresource, linear, row pitch, depth pitch)`, returns `HRESULT` |
| 0x48 | `CopyFrom(linear, plane, subresource, sample, tiled, row pitch, depth pitch)`, returns `HRESULT` |
| 0x50 | `Tiling(tiles, packed, shape, count*, first, entries*)`, returns `HRESULT` |
| 0x58 | view descriptor, not implemented |
| 0x60 | tiled-resource query, not implemented |

`subresource` is `slice * mips + mip`.

## Layout output (0x1B40 bytes)

| Offset | Field |
|---|---|
| +0x00 | `UINT64` texture size in bytes |
| +0x08 | `UINT64` texture alignment |
| +0x10 | `UINT32` mip levels |
| +0x14 | `UINT32` planes |
| +0x18 | 4 plane records of 0x6C8 bytes |
| +0x1B38 | `UINT32` dimension |

Plane record:

| Offset | Field |
|---|---|
| +0x00 | `UINT32` usage (1 data, 7 depth, 8 stencil) |
| +0x08 | `UINT64` size in bytes |
| +0x10 | `UINT64` slice size in bytes |
| +0x18 | `UINT64` offset from the start of the texture |
| +0x20 | `UINT64` alignment |
| +0x28 | `UINT32` bytes per element |
| +0x2C | `UINT32` standard mips |
| +0x30 | `UINT32` tail mips |
| +0x38 | 15 mip records of 0x70 bytes |

Mip record:

| Offset | Field |
|---|---|
| +0x00 | `UINT64` size in bytes |
| +0x08 | `UINT64` offset in bytes |
| +0x10 | `UINT64` slice size in bytes |
| +0x18 | `UINT32` row pitch in texels |
| +0x1C | `UINT32` row pitch in bytes |
| +0x20 | `UINT32` alignment |
| +0x24 | `UINT32` padded width, height, depth or array size |
| +0x30 | `UINT32` width, height |
| +0x38 | `UINT32` texel width, height |
| +0x40 | `UINT32` depth or array size |
| +0x44 | `UINT32` sample count |
| +0x48 | `UINT32` swizzle mode |
| +0x50 | `UINT64` bank rotation mask, bytes per slice |
| +0x60 | `UINT32` slice depth |
| +0x64 | `UINT32` in tail |
| +0x68 | `UINT32` tail position (x bits 0-10, y bits 11-21) |

Widths and heights count elements: texels, or 4x4 blocks for block-compressed formats. The slices of a mip are stored one after another. Records past the mip and plane counts stay zero.

## 64 KiB tiling

`Tiling` reports plane 0 of a 2D 64 KiB layout, in 64 KiB tiles.

| Output | Layout |
|---|---|
| `tiles` | `UINT32` total tiles |
| `packed` | `UINT8` standard mips, `UINT8` tail mips, `UINT32` tail tiles, `UINT32` start |
| `shape` | `UINT32[3]` tile shape in texels |
| `count` | `UINT32` entries in and out |
| `entries` | `UINT32` width, `UINT16` height, `UINT16` depth, `UINT32` start tile |

With a mip tail, at most two slices are listed and entries for slices after the first stay zero.

## Swizzle modes

| Mode | Name | Block |
|---|---|---|
| 0 | linear | none |
| 1, 2 | 256B `S`, `D` | 256 bytes |
| 5, 6, 21, 22, 23 | 4KB `S`, `D`, `S_X`, `D_X`, `R_X` | 4 KiB |
| 9, 10, 17, 18, 24, 25, 26, 27 | 64KB `S`, `D`, `S_T`, `D_T`, `Z_X`, `S_X`, `D_X`, `R_X` | 64 KiB |

3D textures accept 0, 5, 9, 17, 21 and 24 through 27.

## Address configuration

Pipe and bank XOR, and multisample placement, depend on an address configuration. It is not part of the resource description. Callers that know it set it before creating a layout; otherwise the default is used. The default is 16 pipes, 8 packers, a 256-byte pipe interleave and 8 compressed fragments. That default is not a measured value for the console GPU.

Pipes and packers are powers of two, pipes from 1 to 64, packers no larger than the pipe count, and at most four pipes per packer. The pipe interleave is 256, 512, 1024 or 2048 bytes. Compressed fragments are 1, 2, 4 or 8. Any other value is rejected.

Element equations are published for a 256-byte pipe interleave. Other interleaves are stored, and XOR element offsets and copies stay unavailable. The compressed-fragment setting does not change those equations. 4 KiB `R_X` has no element equation.

An element offset is the byte address of a sample from the start of the plane. Coordinates outside the mip, and a sample index at or past the sample count, are rejected. Within a block the address is the XOR of coordinate bits selected by the swizzle mode, element size and address configuration. The block index is then added. A tail mip adds its tail coordinate before that XOR and keeps the block index of the unadjusted coordinate. Samples of one texel are adjacent in a linear copy, sample 0 first. A copy of a multisampled mip carries every sample.
