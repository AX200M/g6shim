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

| Export | Signature and use |
|---|---|
| `AcpHalAllocateShapeContexts` | `HRESULT (void*, void*)`. On failure, audio libraries take their software path. On success they read the hardware contexts through the two pointers. |
