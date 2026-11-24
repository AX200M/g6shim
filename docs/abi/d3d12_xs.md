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
