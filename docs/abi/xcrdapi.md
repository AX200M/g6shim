# XCRDAPI.dll

## Exports

| Export | Notes |
|---|---|
| `DStorageGetFactory` | The only export the title imports. |

## Factory

| Item | Value |
|---|---|
| Factory IID | `62959D11-D45F-4884-A0A3-F7862EA243AE` |

| Offset | Method |
|---|---|
| 0x00 | QueryInterface (unimplemented) |
| 0x08 | AddRef |
| 0x10 | Release |
| 0x18 | 2020 queue creation (unimplemented) |
| 0x20 | OpenFile(path, iid, out) |
| 0x28 | CreateStatusArray(capacity, name, iid, out) |
| 0x30 | 2020 CPU affinity (unimplemented) |
| 0x38 | 2020 debug flags (unimplemented) |
| 0x40 | SetStagingBufferSize(bytes) |
| 0x48 | unimplemented |
| 0x50 | CreateQueue(desc, iid, out) |
| 0x58-0x78 | unimplemented |

## Queue description

| Offset | Field |
|---|---|
| 0x00 | source type, u64 |
| 0x08 | capacity, u16 |
| 0x0A | priority, i8 |
| 0x10 | name, char* |
| 0x18 | unknown, u64 (the title sets 1) |

## Request

0x48 bytes.

| Offset | Field |
|---|---|
| 0x00 | options, u64 |
| 0x08 | destination address, or page numbers with bit 21 |
| 0x10 | page offset, low 16 bits |
| 0x18 | destination size, u32 |
| 0x20 | source file object, or address with bit 22 |
| 0x28 | source offset, u64 |
| 0x30 | source size, u32 |
| 0x34 | intermediate size, u32 |
| 0x38 | tag, u64 |
| 0x40 | name, char* |

| Bit | Meaning |
|---|---|
| 0 | zlib (RFC 1950) |
| 1-4 | BCPack BC format number, 0 none |
| 5-20 | further transform, unimplemented |
| 21 | page destination |
| 22 | memory source |
| 23 | physical-page source, unimplemented |

## Status array

| Offset | Method |
|---|---|
| 0x00 | unimplemented |
| 0x08 | AddRef |
| 0x10 | Release |
| 0x18 | IsComplete(entry) -> bool |
| 0x20 | StatusResult(entry) -> HRESULT |

| Value | Meaning |
|---|---|
| `0x00000000` | done, all requests before the entry succeeded |
| `0x8000000A` | pending |
| `0x80070026` | source ended before its size |
| `0x8007000D` | bad compressed stream |
| `0x80070057` | invalid argument |
| `0x80004001` | unsupported option or no BCPack tables |
| `0x80004002` | unknown factory IID |
| `0x8007000E` | out of memory |

## File

| Offset | Method |
|---|---|
| 0x00 | unimplemented |
| 0x08 | AddRef |
| 0x10 | Release |
| 0x18-0x50 | unimplemented |

## Queue

| Offset | Method |
|---|---|
| 0x00 | unimplemented |
| 0x08 | AddRef |
| 0x10 | Release |
| 0x18 | EnqueueRequest(request) |
| 0x20 | EnqueueStatus(array, entry) |
| 0x28 | 2020 fence signal (unimplemented) |
| 0x30 | Submit() |
| 0x38 | 2020 cancel by tag (unimplemented) |
| 0x40 | Close() |
| 0x48 | 2020 error event (unimplemented) |
| 0x50 | 2020 error record (unimplemented) |
| 0x58 | 2020 queue info (unimplemented) |

## BCPack

| Item | Value |
|---|---|
| Table file location | `G6SHIM_BCPACK_TABLES`, else `bcpack-tables.bin` next to `XCRDAPI.dll` |
| Table file bytes | 77982 |
| Model frequencies | 128 x 9 u16; bucket `b` > 0 holds 2^(b-1) symbols |
| Model shape | model `16 g + shape` uses `g + 2` buckets |
| Rank tables | BC1 256, BC3 alpha 64, BC3 colour 256, BC4 64, BC5 64 |
| BC7 control words | 512 u16 |
| Dictionaries | 255 entries per format |
| Lanes | 12 (BC1, BC3, BC4, BC5) or 16 (BC6, BC7) |
| Record stride | 12 (BC1, BC4), 24 (BC3, BC5), 32 (BC6, BC7) |
| rANS low state | 2^24 |
| Input bytes per round | 8 |
