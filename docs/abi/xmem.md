# xmem.dll

The console's memory library: blocks for the title and its middleware, console-shaped virtual memory, and 64 KiB physical pages the title maps into its own reservations. The GPU side works from CPU addresses (unified memory), so memory types and bandwidth classes change nothing here; what is honoured is what constrains addresses: alignment, 2 MiB pages and the address-width field.

## Exports

| Export | Signature and use |
|---|---|
| `XMemAlloc` | `void* (SIZE_T bytes, UINT64 attributes)`. The title's general allocator, for its middleware too: it calls the routine pair the exe exports as `__XMemAllocationHooks__` when it has one, else `XMemAllocDefault`. |
| `XMemFree` | `void (void* block, UINT64 attributes)`. Dispatches like `XMemAlloc`. |
| `XMemAllocDefault` | `void* (SIZE_T, UINT64)`. The system allocator behind `XMemAlloc`. |
| `XMemFreeDefault` | `void (void*, UINT64)`. Frees what `XMemAllocDefault` returned, given the same attributes. |
| `XMemIsToolsMemAvailable` | `BOOL ()`. Always 0: a retail console has no tools memory. |
| `XMemVirtualAlloc` | `void* (void* base, SIZE_T bytes, DWORD type, UINT64 flags, DWORD protect)`. See below. |
| `XMemVirtualQuery` | `BOOL (HANDLE process, const void* address, BASIC_INFORMATION* out)`. The host's answer for the first 0x30 bytes, then the console tail. |
| `XMemAllocatePhysicalPages` | `BOOL (UINT64 flags, SIZE_T* count, SIZE_T* pages)`. `count` is in/out: pages wanted, pages given. All or nothing. |
| `XMemFreePhysicalPages` | `BOOL (SIZE_T count, const SIZE_T* pages)`. Removes the page numbers; views already mapped stay readable. |
| `XMemMapPhysicalPages` | `void* (void* address, SIZE_T count, const SIZE_T* pages)`. Maps consecutive pages at `address` in place of what is there; null `pages` unmaps. Returns `address`, or null on failure. |
| `XMemMapPhysicalPagesScatter` | `BOOL (void* const* addresses, SIZE_T count, const SIZE_T* pages)`. One page per address; a null `pages` or a page number 0 unmaps. |
| `XMemGetWorkingSetStatistics` | `BOOL (DWORD which, WORKING_SET* out)`. See below. |
| `XMemTransferMemory` | `BOOL (DWORD kind, SIZE_T bytes)`. Rounds up to 2 MiB and moves title memory to the system for kind 0; other kinds and over-budget sizes return 0. |

## XMemAlloc attributes

Bits 24-28 are log2 of the alignment, with values below 16 bytes read as 16; bit 33 asks for a 32-bit address: a whole reservation in the low 4 GiB, freed with the same bit; the remaining bits name a memory type, which changes nothing here.

## XMemVirtualAlloc arguments

`type`: the console adds two bits to the host's allocation type. `0x20000000` asks for 2 MiB pages (the reservation is then aligned to 2 MiB); with `0x400000` it asks for 64 KiB pages instead, and an uncommitted reservation of them becomes a host placeholder that `XMemMapPhysicalPages` replaces part by part. Both bits are stripped before the host sees the type. `protect`: the host's constants below `0x200` (including `PAGE_GUARD`) pass through; the console's GPU bits (`0x800`-`0x40000`) and the no-cache and write-combine bits are dropped; a protection that masks to zero reads as no-access. `flags`: bit 2 asks for 32-bit addresses (the low 4 GiB); bits 12-14 are an address-width field: without it, or with field 7, addresses run to 40 bits, and field 1-6 gives 46 down to 41.

A reservation at g6shim's choice starts at 4 GiB (kept clear of the 32-bit range, which the title's big pools would otherwise crowd out) and ends below the field's limit — the console keeps title memory there, and its ray-tracing structures must be within 40 bits, so a title's builder refuses a higher address. A plain such reservation, or one of 2 MiB pages, is write-watched, so a copy of title memory can take only the pages the title wrote since the last one. A placeholder reports state reserve with protection 0.

## BASIC_INFORMATION (0x40 bytes)

The host's `MEMORY_BASIC_INFORMATION` (0x30 bytes), then the XMem flags (`UINT64`: 3, graphics memory, for a committed region, else 0), then a tag code (`DWORD`, 0) and a 4-byte tag.

## XMemGetWorkingSetStatistics result (0x50 bytes)

| Offset | Field |
|---|---|
| `0x00` | title limit |
| `0x08` | title used |
| `0x10` | tools limit |
| `0x18` | tools used |
| `0x20` | stack used |
| `0x24` | physical pages |
| `0x28` | GPU-optimal limit |
| `0x30` | GPU-optimal used |
| `0x38` | tools GPU-optimal limit |
| `0x40` | tools GPU-optimal used |
| `0x48` | kernel pool used |
| `0x4c` | kernel pool limit |

`which` is ignored and every field is filled. The title limit is the GPU-optimal and standard budgets less memory transferred to the system; title use is the process's pagefile usage.

## Title memory

| Configuration | Console | GPU-optimal | Standard |
|---|---|---|---|
| default | Series X | 10 GiB | 3.5 GiB |
| small | Series S | 0 | 7,776 MiB |

## Physical pages

| Item | Value |
|---|---|
| Page size | 64 KiB |
| Page number unit | 4 KiB frame; a 64 KiB page's number is a multiple of 16 |
| First page number | `0x100000`; page number 0 is never used |
| Title packing | 21 bits as `number >> 4`, rebuilt with `<< 4` |
| Host storage | a pagefile-backed section; page *n* is its *n*-th 64 KiB slice |
| Mapping | a view of consecutive slices replaces a 64 KiB-page placeholder; one page may be mapped at several addresses |
| Freeing | removes the page number; views already mapped stay readable |
