# AcpHal.dll

The audio control processor (ACP) runs the console's audio hardware. Titles reach it through `AcpHal.dll`, either directly or through audio middleware that decodes XMA on the hardware.

## APU memory

The audio hardware addresses memory with 32-bit APU addresses, so APU blocks live below 4 GiB. g6shim maps them one to one: a block's APU address is its virtual address.

| Export | Signature and use |
|---|---|
| `ApuCreateHeap` | `HRESULT (UINT32 cached, UINT32 uncached)`: sizes of the two heaps `ApuAlloc` draws from |
| `ApuAlloc` | `HRESULT (void** va, UINT32* apu, UINT32 size, UINT32 alignment, UINT32 flags)`. `apu` may be null. Observed alignments 4 to 0x800, flags 1 and 2 |
| `ApuFree` | `void (void* va)` |
| `ApuMapVirtualAddress` | `UINT32 (const void* va)`: the APU address of a pointer into a block. Titles write the result into hardware contexts |
| `ApuIsVirtualAddressValid` | `BOOLEAN (const void* va, UINT32 size)`: whether `size` bytes from `va` lie in one block. Titles check buffers with it before mapping them |

## Shape contexts

The hardware keeps one set of context arrays per process, one array per kind of processing block.

| Export | Signature and use |
|---|---|
| `AcpHalAllocateShapeContexts` | `HRESULT (void* config)`. Reads six context counts and writes six array pointers back into `config`. Titles clear the arrays themselves after the call. |
| `AcpHalReleaseShapeContexts` | `HRESULT ()`. Frees the arrays; titles call it after releasing their `IAcpHal` objects. |
| `AcpHalCreate` | `HRESULT (IAcpHal** hal)` |

`config` is 0x60 bytes; titles zero it before filling the counts.

| Offset | Content |
|---|---|
| +0x00 | six `UINT32` context counts, one per kind |
| +0x18 | six array pointers, written by the call; kinds with a count of 0 get null |
| +0x48 | six `UINT32` APU addresses of the arrays, written by the call |

| Kind | Block | Context size | Array alignment | Maximum count |
|---|---|---|---|---|
| 0 | sample-rate converter (SRC) | 32 | 0x20 | 0x1000 |
| 1 | EQ and compressor | 160 | 0x10 | 0x1000 |
| 2 | filter and volume | 32 | 0x20 | 0x1000 |
| 3 | DMA | 16 | 0x10 | 0x1000 |
| 4 | XMA decoder | 64 | 0x40 | 0x200 |
| 5 | PCM | 32 | shares the XMA block | 0xE00 |

- When the SRC or PCM count is not 0, the SRC count must equal the XMA count plus the PCM count: every source has its converter. Otherwise the call fails with `E_INVALIDARG`, as it does for a count above the maximum.
- The PCM array follows the XMA array in one block.
- A second allocation fails with `0x8AC80001`; a release without an allocation fails with `0x8AC80002`.
- Kinds 1 and 2 are named after the hardware blocks by their context sizes; the names of kinds 0 and 3 to 5 match the shared-context fields the console's audio library uses.
