# d3d12_xs.dll

The Series console's Direct3D 12 runtime. Console graphics objects are g6shim objects whose function tables follow the order the title's inlined code expects. GPU addresses are CPU addresses.

## Exports

| Export | Signature and use |
|---|---|
| `D3D12ConfigureVirtualMemoryX` | `HRESULT (D3D12X_VIRTUAL_MEMORY*)`; reports the process's user address range as the GPU range |
| `D3D12XboxCreateDevice` | `HRESULT (adapter, const UINT32* parameters, REFIID, void** device)`; `parameters[0]` is the interface version, `0x20004` observed |
| `D3D12SerializeRootSignature` | PC signature; console flags (bits 24-31) dropped before the host serializes |
| `D3D12SerializeVersionedRootSignature` | PC signature with a leading version word |
| `D3D12CreateRootSignatureDeserializer` | `HRESULT (const void* blob, SIZE_T size, REFIID, void** out)`; the host's deserializer reads the blobs above |
| `IID_ID3D12Device` | data: the 16-byte device interface id, also imported by titles |

## Objects

| Offset | Field |
|---|---|
| +0x00 | function table |
| +0x08 | `UINT32` device index and bookkeeping bits |
| +0x0C | `UINT32` reference count |

| Object | Bytes | Table slots |
|---|---|---|
| device | 0x58 (`+0x10` null constant buffer, `+0x18` out-of-line flags, first word 1) | 160, `ID3D12Device` order then console additions |
| DXGI device | 0x10 | 32: 3 IUnknown, 4 object, 5 device, 2 `IDXGIDevice` |
| DXGI adapter | 0x10 | 24: 3, 4, 3 `IDXGIAdapter`, 1 `IDXGIAdapter1`, 1 `IDXGIAdapter2` |
| DXGI factory | 0x10 | 32: 3, 4, 5 `IDXGIFactory`, 2 `IDXGIFactory1`, 11 `IDXGIFactory2` |
| DXGI output | 0x10 | 32: 3, 4, 12 `IDXGIOutput`, 3 console |
| heap | 0x10 + 0x30 + memory | 24: 3, 4, 1 device child, 1 `ID3D12Heap` |
| resource | `Object` + description + properties + backing | 32: 3, 4, 1, 7 `ID3D12Resource`, 3 console |
| fence | `Object` + value + flags + waiters | 24: 3, 4, 1, 3 `ID3D12Fence`, 1 console (flags), 1 console (GPU address) |
| descriptor heap | `Object` + description + start + memory | 24: 3, 4, 1, 3 `ID3D12DescriptorHeap` |
| root signature | `Object` + serialized blob | 24: 3, 4, 1 |
| query heap | `Object` + description | 24: 3, 4, 1 |
| command signature | `Object` + stride + arguments | 24: 3, 4, 1 |

The object methods occupy slots 3 to 6: `GetPrivateData`, `SetPrivateData`, `SetPrivateDataInterface` (unwritten), `SetName`.

## Device slots

| Slot | Offset | Method |
|---|---|---|
| 0 | 0x00 | `QueryInterface` |
| 7 | 0x38 | `GetNodeCount` |
| 8 | 0x40 | `CreateCommandQueue` (later part) |
| 9 | 0x48 | `CreateCommandAllocator` (later part) |
| 10, 11 | 0x50, 0x58 | `CreateGraphicsPipelineState`, `CreateComputePipelineState` (later part) |
| 12 | 0x60 | `CreateCommandList` (later part) |
| 13 | 0x68 | `CheckFeatureSupport` |
| 14, 15 | 0x70, 0x78 | `CreateDescriptorHeap`, `GetDescriptorHandleIncrementSize` |
| 16 | 0x80 | `CreateRootSignature` |
| 17-22 | 0x88-0xb0 | `CreateConstantBufferView` to `CreateSampler` |
| 23, 24 | 0xb8, 0xc0 | `CopyDescriptors`, `CopyDescriptorsSimple` |
| 25, 26 | 0xc8, 0xd0 | `GetResourceAllocationInfo`, `GetCustomHeapProperties` |
| 27-29 | 0xd8-0xe8 | `CreateCommittedResource`, `CreateHeap`, `CreatePlacedResource` |
| 36 | 0x120 | `CreateFence` |
| 38 | 0x130 | `GetCopyableFootprints` |
| 39 | 0x138 | `CreateQueryHeap` |
| 41 | 0x148 | `CreateCommandSignature` |
| 48 | 0x180 | `CreateCommandQueueX` (later part) |
| 61 | 0x1e8 | `CreateSamplerX` |
| 62 | 0x1f0 | `CreateComputePipelineStateX` (later part) |
| 69 | 0x228 | debug error filter, accepted and ignored |
| 70, 71 | 0x230, 0x238 | `CreateShaderResourceViewX`, `CreateUnorderedAccessViewX` of placed memory |
| 73 | 0x248 | `CreateCommandListX` (later part) |
| 74, 75 | 0x250, 0x258 | `RegisterPagePool`, `UnregisterPagePool` |
| 76 | 0x260 | `CreateCommittedResourceX` |
| 78-81 | 0x270-0x288 | `SetFrameIntervalX`, `ScheduleFrameEventX`, `WaitFrameEventX`, `GetFrameStatisticsX` |
| 86 | 0x2b0 | `CreateStreamPipelineState` (later part) |
| 108 | 0x360 | `GetResourceAllocationInfo2` |
| 127 | 0x3f8 | `CreateCommittedResource3` |
| 133 | 0x428 | `CreateResourceAtAddress` |

## Interface ids

| Object | IIDs |
|---|---|
| device | `189819f1-1db6-4b57-be54-1821339b85f7`, then `ID3D12Device1` to `ID3D12Device8` |
| DXGI device | `54ec77fa-1377-44e6-8c32-88fd5f44c84c`, `77db970f-6276-48ba-ba28-070143b4392c`, `05008617-fbfd-4051-a790-144884b4f6a9` |
| adapter | `2411e7e1-12ac-4ccf-bd14-9798e8534dc0`, `29038f61-3839-4626-91fd-086879011a05`, `0aa1ae0a-fa0e-4b84-8644-e05ff8e5acb5` |
| factory | `7b7166ec-21c7-44ae-b21a-c9ae321ae369`, `770aae78-f26f-4dba-a829-253c83d1b387`, `50c83a1c-e072-4c48-87b0-3630fa36a6d0` |
| output | `ae02eedb-c735-4690-8d52-5a8dc20213aa` |
| heap (resource) | `6b3b2502-6e51-45b3-90ee-9884265e8df3` |
| resource | `696442be-a72e-4059-bc79-5b5c98040fad` |
| fence | `0a753dcf-c4d8-4b91-adf6-be5a60d95a76` |
| descriptor heap | `8efb471d-616c-4f49-90f7-127bb763fa51` |
| root signature | `c54a6b66-72df-4ee8-8be5-a946a1429214` |
| query heap | `0d9658ae-ed45-469e-a61d-970ec583cab4` |
| command signature | `c36a797c-ec80-4f0a-8985-a7b2475082d1` |

Every device child also answers for `63ee58fb-1268-4835-86da-f008ce62f0d6` (`ID3D12Pageable`), `905db94b-a00c-4140-9df5-2b64ca9ea357` (`ID3D12DeviceChild`) and `c4fec28f-7966-4e95-9f94-f431cb56c3b8` (`ID3D12Object`).

## Structures

| Structure | Bytes | Fields |
|---|---|---|
| `D3D12X_VIRTUAL_MEMORY` | 0x20 | flags, page-table pages, preserved bytes, `UINT64 lowest`, `UINT64 highest` |
| device options | 0x40 | 16 `UINT32`; word 11 is console-only, word 14 viewport/RT index from any stage, word 15 resource heap tier 2 |
| feature-level query | 0x18 | count, requested levels, highest supported out |
| architecture query | 0x10 | node in, tile-based, unified memory, cache-coherent out |
| format-support query | 0x0C | format in, support 1 out, support 2 out |
| multisample query | 0x10 | format, sample count in, quality levels out |
| `DXGI_ADAPTER_DESC` as listed | 0x130 | name 128 UTF-16, vendor, device, subsystem, revision, three `SIZE_T` memory sizes, LUID |
| display mode | 0x20 | width, height, refresh numerator/denominator, format, scanline order, scaling, flags |
| heap properties | 0x14 | kind, CPU page property, memory pool, creation node mask, visible node mask |
| heap description | 0x30 | size, properties, alignment, flags |
| resource description | 0x38 | dimension, alignment, width, height, depth or array size (`UINT16`), mips (`UINT16`), format, sample count, quality, layout, flags |
| resource description 1 | 0x48 | description then sampler-feedback mip region |
| allocation info | 0x10 | size, alignment |
| allocation info 1 | 0x18 | offset, alignment, size |
| placed footprint | 0x20 | offset, format, width, height, depth, row pitch |
| descriptor heap description | 0x10 | kind, count, flags, node mask |
| descriptor placement (after the description) | 0x10 | address, bytes |
| sampler description (console) | 27 words | flags, mag, min, mip, depth, mode, 3 address words, min/max LOD, bias, secondary bias, anisotropy, anisotropy bias, threshold, compare, 4 border words, tuning words |
| root signature description | 0x28 | parameter count, parameters, sampler count, samplers, flags |
| versioned root signature description | 0x30 | version, then the description |
| command signature description | 0x18 | stride, count, arguments (each 4 words), node mask |
| query heap description | 0x0C | kind, count, node mask |

## Descriptor memory

| Kind | Bytes per descriptor |
|---|---|
| 0 views | 32 |
| 1 samplers | 16 |
| 2 render targets | 128 |
| 3 depth | 128 |

| Tag | First word | Record |
|---|---|---|
| view | `vVIE` | tag, entry index, generation |
| render target view | `vRTV` | tag, described, resource, 6 description words |
| depth view | `vDSV` | as a render target view, tag `vDSV` |

## Frame events

| Kind | Event |
|---|---|
| 0 | frame origin; makes the frame token |
| 1 | vertical blank |
| 2 | flip |

Wait flag 2 makes a token when the wait times out. The token counts frames; ticks count intervals.

## Host GPU

| Piece | Value |
|---|---|
| device | one `ID3D12Device`, feature level 12_0, the default adapter or WARP with `G6SHIM_D3D_WARP` |
| debug layer | enabled when `G6SHIM_D3D_DEBUG` is set |
| queue | one direct queue, submissions waited for on a fence and an event |
| render target ring | 1024 descriptors of the RTV heap |
| depth ring | 1024 descriptors of the DSV heap |
| swap chain | 2 buffers, flip discard, `Present(0, 0)` |
| drawable formats | 27-32, 87, 90, 91, 23-25, 9-14; other formats are not shown |

## Host lists

| Piece | Bytes |
|---|---|
| scratch chunk | 4 MiB, allocations rounded to 256 |
| scratch chunk over the limit | its own upload buffer |
| indirect-work budget | one 256-byte shared buffer, zeroed at each submission |
| snapshot pool | at most 8 buffers, the largest kept |

| Step | Meaning |
|---|---|
| copy | title memory to host memory at submission, clearing refresh state |
| fill | title memory into the list's upload memory at submission |
| refresh | host copy range brought up to date once per submission until a title write |
| work | a function run at submission, optionally counted as a title write |
| GPU write | shaders may write a host copy range; snapshot before, copy back after |
| texture | a texture's staging buffer detiled once per submission |

## Host descriptors

| Piece | Value |
|---|---|
| host slots behind title slots | same index; views get 65536 extra dynamic slots |
| host heap creation | made on the first draw that uses the title heap; title writes before wait in a pending list |
| portable view heap limit | 1000000 descriptors; larger title heaps are capped |
| dynamic search | from the slot after the last allocation, runs never wrap |
| null fill | one null slot, doubled by descriptor copies to a 65536-slot block |
| null depth, unordered views | texture 1D, 1D array, 2D, 2D array according to the title's dimension, WSize `UINT_MAX` for 3D |

## Title memory copies

| Piece | Bytes |
|---|---|
| tracked and copied unit | 64 KiB |
| kernel write-watch unit | 4 KiB |
| loose memory window | 4 MiB aligned, length rounded to the window |
| shadow buffer rounding | 64 KiB, so views ending in the last 256 bytes fit |

| State | Meaning |
|---|---|
| watch 0 | not asked yet |
| watch 1 | write-watched through the kernel |
| watch 2 | not write-watched; whole ranges are copied or hashed |

## Texture staging

| Piece | Meaning |
|---|---|
| footprints | the host's copyable footprints for every mip and array slice |
| description | console plane 0 layout, subresource index `slice * mips + mip` |
| refresh | written pages since the last asking detile the whole texture; unwatched memory hashed |
| upload | one `CopyTextureRegion` per subresource, from the staging buffer |

## Xmem private exports

| Export | Signature |
|---|---|
| `g6shim_xmem_map_alias` | `BOOL (uintptr_t to, uintptr_t from, size_t bytes)`; maps title physical pages at `from` (zero: unmaps) |

## Ray-tracing acceleration structures

Translated ray-tracing shaders walk acceleration structures themselves, so a build writes the console's BVH4 records, not a host acceleration structure. `hle/d3d12_xs/bvh/` builds them with compute shaders. Every record offset counts from the structure's start, so a structure can be copied byte for byte.

### Build inputs

`BuildRaytracingAccelerationStructure` takes the public `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC` (0x30 bytes) as the first part of the console's batched description (0x50 bytes):

| Offset | Bytes | Field |
|---|---|---|
| 0x00 | 8 | destination address; 256-byte aligned |
| 0x08 | 24 | inputs: type (0 top level, 1 bottom level, others refused), flags, count, layout, descriptions |
| 0x20 | 8 | source address (updates) |
| 0x28 | 8 | scratch address |
| 0x30 | 4 | postbuild description count |
| 0x38 | 8 | postbuild descriptions (16 bytes each) |
| 0x40 | 16 | stream description: size, pointer to 16-byte records |

| Flag | Effect |
|---|---|
| 0x01 `ALLOW_UPDATE` | the structure can be refitted; sibling triangles are not paired |
| 0x04 `MINIMIZE_MEMORY` | bottom level: inner nodes are BOX16 |
| 0x20 `PERFORM_UPDATE` | refit the source (or the destination) to new positions; needs `ALLOW_UPDATE` |

Prebuild sizes depend only on the element count (triangles or instances), the level and the node kind: the result has room for the most nodes a tree of that many elements can hold, `(3n - 2) / 4` for `n > 1`, one otherwise. Update scratch equals build scratch. Postbuild `COMPACTED_SIZE` (0) and `CURRENT_SIZE` (3) both report `64 * (totalSizeIn64BNodes + 1)`; type `0x10` (offline statistics) is refused with `E_INVALIDARG`.

### Regions

| Region | Offset | Size |
|---|---|---|
| header | 0 | 64 |
| parent table | 64 | one `UINT32` per 64-byte unit from the elements on, rounded up to 64 bytes |
| elements | after the parent table | 64 per triangle leaf, 128 per instance |
| nodes | after the elements | 128 per BOX32 node, 64 per BOX16 node; a BOX16 structure ends with one zeroed spare unit |

### Header (64 bytes)

| Offset | Field |
|---|---|
| 0x00 | flags: `0x80000000` bottom level, low bits the version 6 |
| 0x04 | `totalSizeIn64BNodes`: size in 64-byte units, not counting the header |
| 0x08 | crc32; 0 for runtime builds |
| 0x0C | element count |
| 0x10 | bounds: min x y z, max x y z (floats) |
| 0x28 | offset of the nodes |
| 0x2C | offset of the primitive remap; 0 |
| 0x30 | root node pointer |
| 0x34 | offset of the elements |
| 0x38 | offset of the parent table |
| 0x3C | compression parameters; 0 |

### Node pointers

A pointer is `(offset / 64) << 3 | type`; `0xFFFFFFFF` is no child.

| Type | Record |
|---|---|
| 0 TRI0 | triangle leaf, vertex slots 0, 1, 2 |
| 1 TRI1 | the same leaf, vertex slots 1, 3, 2 |
| 2 TRI2, 3 TRI3 | not written |
| 4 BOX16 | 64-byte node with half-float boxes |
| 5 BOX32 | 128-byte node with float boxes |
| 6 INSTANCE | 128-byte instance record |
| 7 AABB | not written |

### Triangle leaf (64 bytes)

| Offset | Field |
|---|---|
| 0x00 | four vertex slots, 12 bytes each (x y z floats) |
| 0x30 | geometry index << 8 \| geometry flags (bit 0 opaque) |
| 0x34 | primitive index of TRI0 |
| 0x38 | primitive index of TRI1, `0xFFFFFFFF` without one |
| 0x3C | barycentric remap: byte 0 for TRI0, byte 1 for TRI1; bits 1:0 and 3:2 are the slots of the triangle's vertices 1 and 2; 0x09 is the identity |

A triangle with a NaN coordinate, or an index at or past the vertex count, is inactive: its box is empty and no ray reaches it. A paired TRI1 shares an edge with its TRI0 in the opposite direction, comes from the same geometry, and sits under the same node.

### Instance record (128 bytes)

| Offset | Field |
|---|---|
| 0x00 | object-to-world 3x4 row-major |
| 0x30 | instance id (24 bits) \| mask << 24 |
| 0x34 | contribution (24 bits) \| flags << 24 |
| 0x38 | BLAS root pointer; bit 31 set when the rotation is the identity, bit 30 scaled local space |
| 0x3C | BLAS address >> 8, as the title wrote it |
| 0x40 | world-to-object 3x4 row-major; zero when the transform is singular |
| 0x70 | instance index |
| 0x74 | parent node pointer |
| 0x78 | 8 zero bytes |

An instance with mask 0, no BLAS or a singular transform has an empty box.

### Box nodes

| Node | Bytes | Layout |
|---|---|---|
| BOX32 | 128 | four child pointers, four boxes of six floats (min x y z, max x y z), 16 zero bytes |
| BOX16 | 64 | four child pointers, four boxes of three words: min x \| min y << 16, min z \| max x << 16, max y \| max z << 16 |

An unused child slot holds `0xFFFFFFFF` and an empty box (min `+inf`, max `-inf`). BOX16 halves round outward: minima toward `-inf`, maxima toward `+inf`; values past the half range become `±inf` outward and `±65504` inward, and magnitudes below `2^-14` become 0 or `±2^-14`, never a denormal.

### Parent table

Entry `u` holds the pointer of the node whose child starts at 64-byte unit `u` counted from the elements' offset; the root's entry and units that start no record hold `0xFFFFFFFF`.

### Host bindings

The builder binds memory through root unordered-access views at `host_base + console address`, so every buffer it reads or writes (destination, scratch, source, vertex, index and transform buffers, instance descriptions and the bottom-level structures they name) must be in the unordered-access state. A bottom-level structure named by an instance is read through one view at `host_base`, so its console address must lie below 4 GiB and inside the address space; otherwise the instance has no BLAS. A build replaces the command list's compute root signature, pipeline and root arguments.
