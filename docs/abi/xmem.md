# xmem.dll

The console's memory allocation library.

| Export | Signature and use |
|---|---|
| `XMemAlloc` | `void* (SIZE_T bytes, UINT64 attributes)`. The console's XG library allocates its texture computers with it. |
| `XMemFree` | `void (void* block, UINT64 attributes)` |

The attribute bits are not decoded.
