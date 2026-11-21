# xg_x.dll

The console's texture layout library. Titles ask it how a texture is laid out in memory before they write texel data themselves. Observed with 1738CA2A (GDK build), and compared with the console library's results. g6shim reproduces the console's layout for tile mode 13 and describes other textures as linear, with the row pitches and placements the PC copy functions need.

## Exports

| Export | Signature and use |
|---|---|
| `XGComputeOptimalTileMode` | `UINT (dimension, format, width, height, array size, sample count, bind flags, misc flags)`; 1738CA2A passes dimension 3 (2D) and bind flags 8 (shader resource). The console answers 13 for single-sample 2D textures of every size and format tried; g6shim does the same and answers 8 (linear general) otherwise. |
| `XGCreateTexture2DComputer` | `HRESULT (const desc*, computer**)` |

## Texture 2D description (0x3C bytes)

`UINT32` fields in this order: width, height, mip levels (0 for the full chain), array size, format (DXGI values), sample count, sample quality, usage, bind flags, CPU access flags, misc flags, ESRAM offset, ESRAM usage, tile mode (+0x34), pitch (+0x38).

## Texture computer

The object starts with a function table. Every function takes the computer as its first argument.

| Slot | Function |
|---|---|
| 0 | `AddRef` |
| 1 | `Release` |
| 2 | `GetResourceLayout(layout*)`, returns `HRESULT` |

The console table has at least 12 slots.

## Resource layout (0x1740 bytes)

| Offset | Field |
|---|---|
| +0x00 | `UINT64` size in bytes |
| +0x08 | `UINT64` base alignment |
| +0x10 | `UINT32` mip levels |
| +0x14 | `UINT32` planes |
| +0x18 | 4 plane records of 0x5C8 bytes |
| +0x1738 | `UINT32` dimension (3 for 2D) |

Plane record:

| Offset | Field |
|---|---|
| +0x00 | `UINT32` usage (1 for colour data) |
| +0x08 | `UINT64` size in bytes |
| +0x10 | `UINT64` offset of the plane |
| +0x18 | `UINT64` alignment |
| +0x20 | `UINT32` bytes per element |
| +0x28 | 15 mip records of 0x60 bytes |

Mip record:

| Offset | Field |
|---|---|
| +0x00 | `UINT64` size of all slices |
| +0x08 | `UINT64` offset from the start of the resource |
| +0x10 | `UINT64` size of one slice |
| +0x18 | `UINT32` row pitch in texels |
| +0x1C | `UINT32` row pitch in bytes |
| +0x20 | `UINT32` alignment |
| +0x24 | `UINT32` padded width, height, depth or array size (three fields) |
| +0x30 | `UINT32` width, height, depth or array size (three fields) |
| +0x3C | `UINT32` sample count |
| +0x40 | `UINT32` tile mode |
| +0x48 | `UINT64` bank rotation address mask |
| +0x50 | `UINT64` bank rotation bytes per slice |
| +0x58 | `UINT32` slice depth |

Widths and heights count elements: 4×4 blocks for block-compressed formats, whose bytes per element is the block size. The row pitch in texels counts texels. The slices of a mip are stored one after another. Records beyond the mip and plane counts are zero. 1738CA2A reads each mip's offset to stream mips separately.

## Tile mode 13

Tile mode 13 lays a texture out in tiles of 8×8 elements (texels, or 4×4 blocks for block-compressed formats). Tiles run row by row across the padded width. Inside a tile, the element at (x, y) is number `x0 | y0<<1 | x1<<2 | y1<<3 | x2<<4 | y2<<5`, where `xn` and `yn` are bit n of x and y. Its byte offset is that number times the element size.

Padding of a level:

- Without mips, width and height are rounded up to 8 elements.
- With more than one mip, the width of level n is `next power of two of the base width` shifted right n times, and its height is the next power of two of the level's own height. Both are then rounded up to 8 elements, and the array size is rounded up to a power of two.
- Then the width grows by 8 elements until one slice is a multiple of 256 bytes.

Levels follow each other with no gap. Within a level, each array slice is complete before the next. The row pitch in bytes is the padded width times the element size, and alignments are 256 bytes.

1738CA2A reads texture files into the memory of its textures in this layout, without converting them.
